mini-redis-c

一个用 C 从零搭出来的 Redis-like 内存键值数据库学习项目。

它不是把一个 HashMap 包在 main.c 里就结束，而是沿着真实服务端的路径，把下面这些模块一步步串起来：

TCP 服务端 / 客户端

poll 多连接事件循环

非阻塞 Socket

自定义二进制消息协议

命令解析与响应类型系统

哈希表 + 链表冲突处理

扩容 / rehash

SET / GET / DEL / EXISTS

EXPIRE / TTL 过期语义

SAVE 快照持久化与启动恢复

当前主线大致处于 ch08：已经形成一个“客户端 -> 网络 -> 协议 -> 命令 -> 内存 DB -> 持久化 -> 响应”的最小闭环。

准确边界说明： 当前代码使用的是项目自定义的长度前缀二进制协议，并不是 Redis 官方 RESP；当前持久化实现是手动 SAVE 的快照，不是 AOF。仓库里会明确保留这些真实边界，不把“后续计划”写成“已经实现”。

1. 先看整体：这个项目运行时到底像什么？

如果完全不看代码，可以先把它想象成一家“内存仓库”。

client：来办业务的人，把 SET foo bar 这样的命令交给服务端。

tcp_server.c：前台 + 调度中心，负责接入连接、收请求、分发命令、回响应。

codec.c / proto.c：翻译和装箱层，把“人输入的命令”变成网络字节，再把网络字节恢复成结构化命令。

db.c：真正的仓库，负责 key/value 在内存中的查找、写入、删除、过期和 rehash。

persist.c：账本，负责 SAVE 时把当前有效数据保存到 my_redis.dump，并在下次启动时恢复。

flowchart LR
    U[终端输入<br/>SET foo bar] --> C[tcp_client.c<br/>拆参数 + 编码请求]
    C --> P1[proto.c<br/>加 4 字节消息长度]
    P1 --> TCP[(TCP 字节流)]
    TCP --> S[tcp_server.c<br/>poll + Conn 状态机]
    S --> CO[codec.c<br/>parse_req]
    CO --> D{do_request<br/>命令分发}
    D -->|SET/GET/DEL/EXISTS/EXPIRE/TTL| DB[db.c<br/>哈希表数据库]
    D -->|SAVE| PS[persist.c<br/>快照保存]
    DB --> R[Resp<br/>OK / ERR / STR / INT / NIL]
    PS --> R
    R --> CO2[codec.c<br/>make_resp]
    CO2 --> S2[tcp_server.c<br/>非阻塞写回]
    S2 --> TCP2[(TCP)]
    TCP2 --> CL[client<br/>parse_resp + print_resp]

一条命令从你按下回车开始，会真实经过：

你输入命令
   ↓
client 把一行文字拆成 argc / argv
   ↓
编码成二进制 request payload
   ↓
前面再加 4 字节 body 长度
   ↓
通过 TCP 发给 server
   ↓
server 的 poll 发现这个连接“可读”
   ↓
Conn 状态机先读 4 字节长度，再读完整 body
   ↓
parse_req 把 body 还原成 Args
   ↓
do_request 根据 argv[0] 判断是什么命令
   ↓
进入 db.c 或 persist.c
   ↓
得到一个 Resp 语义结果
   ↓
make_resp 把结果重新编码成网络字节
   ↓
poll 等待 socket 可写
   ↓
server 分段写回 client
   ↓
client 解析 RespView
   ↓
终端看到 < OK / < bar / < 1 / < (nil)

2. 当前已经实现了什么？

层

已实现能力

当前实现方式

网络层

TCP Server / Client

IPv4 + SOCK_STREAM

多连接

同时维护多个客户端

poll + 最多 1024 个 Conn 槽位

非阻塞 I/O

accept/read/write 不拖死整个事件循环

fcntl(..., O_NONBLOCK)

消息边界

解决 TCP 没有消息边界的问题

[4-byte body_len][body]

请求编解码

命令参数序列化

[argc][arg_len][arg_bytes]...

响应类型

区分成功、错误、字符串、整数、空值

RESP_OK / ERR / STR / INT / NIL

KV 存储

字符串 key/value

哈希桶 + 链表

哈希冲突

多个 key 落入同一桶

separate chaining

扩容

降低桶链越来越长的问题

负载超过约 75% 时整体 rehash

删除

删除 key 并释放资源

db_del + 链表摘节点

存在性

判断 key 是否存在

EXISTS / db_exists

过期

设置 TTL

EXPIRE key seconds

TTL 查询

查询剩余秒数

TTL，支持 -1 / -2 语义

过期清理

访问时清理已过期 key

lazy expiration

持久化

手动保存当前有效数据

SAVE -> my_redis.dump

启动恢复

服务启动时尝试恢复快照

persist_load

学习轨迹

保留每章演进

docs/ + archive/

当前命令集：

SET key value
GET key
DEL key
EXISTS key
EXPIRE key seconds
TTL key
SAVE

3. 服务端宏观画面：一个单线程事件循环如何照看很多连接？

3.1 启动时

./server 启动后会依次做：

socket()
  ↓
setsockopt(SO_REUSEADDR)
  ↓
bind(0.0.0.0:15001)
  ↓
listen()
  ↓
db_init(&g_db)
  ↓
persist_load(&g_db, "my_redis.dump")
  ↓
把监听 socket 设为 NONBLOCK
  ↓
进入 while(1) + poll(...)

可以把它想成：

先开店门
→ 再把仓库初始化
→ 再把昨天的快照恢复回来
→ 最后坐进总控室，等操作系统通知“哪个连接有事了”

服务端监听端口：

0.0.0.0:15001

客户端默认连接：

127.0.0.1:15001

3.2 为什么不是每个连接都一直 read()？

因为那样一个慢客户端就可能卡住整个服务。

当前服务端做的是：

poll：谁准备好了？
   ↓
连接 A 可读 → 只处理 A
连接 B 可写 → 只处理 B
监听 fd 可读 → accept 新连接
其余连接没事件 → 不碰

flowchart TD
    LOOP[while 1] --> BUILD[构造 pollfd 列表]
    BUILD --> POLL[poll 等待事件]
    POLL --> L{监听 fd 可读?}
    L -->|是| AC[accept 尽可能多的新连接]
    L -->|否| E[检查已有客户端]
    AC --> E
    E --> R{当前 Conn 状态}
    R -->|ST_READ_LEN / ST_READ_BODY| READ[关心 POLLIN]
    R -->|ST_WRITE_RESP| WRITE[关心 POLLOUT]
    READ --> LOOP
    WRITE --> LOOP

4. 微观核心：Conn 就是一张“这个客户端办到哪一步”的进度卡

非阻塞 I/O 最关键的地方，是一次 read 不保证把一条请求读完整，一次 write 也不保证把响应一次发完。

所以每个客户端都有一份 Conn：

typedef struct {
    int fd;
    ConnState st;

    uint8_t lenbuf[4];
    size_t len_got;
    uint32_t body_len;
    uint8_t *body;
    size_t body_got;

    uint8_t *wbuf;
    size_t wlen;
    size_t wsent;
} Conn;

可以把它脑补成下面这张表：

客户端 #17
┌────────────────────────────────────┐
│ fd        = 17                     │
│ state     = ST_READ_BODY           │
│ len_got   = 4 / 4                  │ ← 长度已经收齐
│ body_len  = 23                     │ ← 这条请求一共 23 字节
│ body_got  = 11 / 23                │ ← 目前只收到了 11 字节
│ wbuf      = NULL                   │ ← 还没执行完，没有响应
│ wsent     = 0                      │
└────────────────────────────────────┘

下一轮 poll 再告诉服务端“17 号连接可读”时，程序不会从头开始，而是直接从 body + 11 继续收剩下的 12 字节。

状态只有三个：

ST_READ_LEN
    │  收齐 4 字节长度
    ▼
ST_READ_BODY
    │  收齐完整 body + 执行命令
    ▼
ST_WRITE_RESP
    │  响应全部写完
    └──────────────────────► ST_READ_LEN

这就是当前项目处理 半包、部分写、非阻塞连接进度 的核心。

5. TCP 没有“消息”概念，所以协议必须自己划边界

当前项目不是 Redis 官方 RESP，而是自己实现了一套更适合学习 TCP framing 的二进制协议。

最外层：

┌──────────────────────┬──────────────────────────────┐
│ 4 bytes body length  │ body                         │
└──────────────────────┴──────────────────────────────┘

例如网络告诉你 body 长度是 27，服务端就知道：

前 4 字节：只负责告诉我后面有多少字节
后 27 字节：才是一整条完整请求

这样就不会错误地假设：

一次 read == 一条命令   ❌

实际可能是：

read #1 → 只收到长度字段前 2 字节
read #2 → 再收到长度字段 2 字节 + body 前 6 字节
read #3 → 收到剩下 body

Conn.len_got 和 Conn.body_got 就是专门记这个进度的。

6. 一条 SET foo bar 在网络里到底长什么样？

客户端当前把一行输入按空格切开：

SET foo bar

得到：

argc = 3
argv[0] = "SET"
argv[1] = "foo"
argv[2] = "bar"

请求 payload：

┌────────────┐
│ argc = 3   │ 4 bytes
├────────────┤
│ len = 3    │ 4 bytes
├────────────┤
│ "SET"      │ 3 bytes
├────────────┤
│ len = 3    │ 4 bytes
├────────────┤
│ "foo"      │ 3 bytes
├────────────┤
│ len = 3    │ 4 bytes
├────────────┤
│ "bar"      │ 3 bytes
└────────────┘

然后 proto.c 在整个 payload 前面再套一层：

[4-byte payload length][argc][len][SET][len][foo][len][bar]

所有 4 字节整数都使用网络字节序。

服务端收齐 body 后，parse_req 不复制参数内容，而是得到一个视图：

Args
├── argc = 3
├── argv[0] ───────► body 中的 "SET"
├── argv[1] ───────► body 中的 "foo"
└── argv[2] ───────► body 中的 "bar"

这也是为什么 db_set 不能直接保存 argv[i] 的指针：

请求处理结束
  ↓
Conn.body 会被释放
  ↓
如果 DB 只是存了指针
  ↓
数据库里就会留下悬空指针

因此 db_set 会重新 malloc 并复制 key/value，数据库自己拥有它们的生命周期。

7. 命令层：网络世界和数据库世界之间的“总机”

服务端把 request 解析成 Args 后进入：

do_request(&args, &resp)

可以把它看成一个交换机：

argv[0]
   │
   ├── SET ───────► db_set
   ├── GET ───────► db_get
   ├── DEL ───────► db_del
   ├── EXISTS ────► db_exists
   ├── EXPIRE ────► db_expire
   ├── TTL ───────► db_ttl
   └── SAVE ──────► persist_save

数据库层返回状态后，命令层不会直接打印字符串，而是先形成一个“语义响应对象” Resp：

RESP_OK   → 操作成功，没有额外数据
RESP_ERR  → 真正错误
RESP_STR  → GET 找到了字符串
RESP_INT  → DEL / EXISTS / TTL 等整数结果
RESP_NIL  → GET 正常执行，但 key 不存在

这里一个重要设计点是：

GET missing

不是“错误”，而是“查询结果为空”，所以是 RESP_NIL，不是 RESP_ERR。

8. 内存数据库：不是数组，而是“桶数组 + 冲突链表”

核心结构：

typedef struct DbNode {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
    bool has_expire;
    int64_t expire_at_ms;
    struct DbNode *next;
} DbNode;

typedef struct {
    DbNode *buckets[2048];
    size_t bucket_count;
    size_t size;
} Db;

初始化时：

bucket_count = 256
size = 0

脑海里的真实画面：

buckets

[0] ───► NULL
[1] ───► [key="foo", val="bar"] ───► [key="user", val="alice"] ───► NULL
[2] ───► NULL
[3] ───► [key="count", val="8"] ───► NULL
...
[255] ─► NULL

为什么一个桶里会有多个节点？

因为：

hash(foo) % bucket_count
hash(user) % bucket_count

可能刚好算出同一个桶下标。

这叫 hash collision。

当前项目不覆盖旧节点，而是用链表挂在同一个 bucket 下面。

key 怎么找到桶？

当前哈希函数使用 FNV-1a 风格：

key 的每个字节
   ↓ XOR
hash
   ↓ × 常量
新的 hash
   ↓
重复直到 key 结束
   ↓
hash % bucket_count
   ↓
桶下标

即使已经定位到桶，也不能直接认为找到 key：

“落在同一个桶” ≠ “是同一个 key”

还必须沿链表逐个检查：

key_len 相同
AND
memcmp(key bytes) 相同

9. SET 的微观逻辑：写一个 key 时到底发生了什么？

以：

SET foo bar

为例。

flowchart TD
    A[db_set foo bar] --> B{插入后负载会 > 75%?}
    B -->|是| RH[rehash 到更多桶]
    B -->|否| H[计算 foo 的桶下标]
    RH --> H
    H --> F[沿桶链查找 foo]
    F --> X{找到仍有效节点?}
    X -->|是| U[复制新 value<br/>释放旧 value<br/>覆盖 val]
    U --> E[清除旧过期时间]
    X -->|否| C[复制 key / value]
    C --> N[malloc DbNode]
    N --> I[头插到 bucket 链表]
    I --> S[db.size++]

如果是覆盖已有 key：

旧节点：foo -> old
          TTL = 10s

SET foo new

结果：foo -> new
      has_expire = false

也就是说，当前语义里重新 SET 会清掉旧 TTL。

10. 为什么要 rehash？

如果一直只有 256 个桶，但越来越多 key 塞进来，会出现：

bucket[7]
   ↓
node A
   ↓
node B
   ↓
node C
   ↓
node D
   ↓
node E
   ↓
...

这时所谓的“哈希查找”最后会越来越像在链表里扫描。

当前实现会在“下一次插入后元素数超过桶数约 3/4”时尝试扩容。

256 buckets
   ↓ 负载升高
512 buckets
   ↓
1024 buckets
   ↓
2048 buckets（当前上限）

rehash 不是重新复制所有 key/value，而是：

旧 bucket 数 = 256
        ↓
遍历所有 DbNode
        ↓
用新的 bucket_count 再算一次下标
        ↓
重新连接 next 指针
        ↓
节点本身继续复用

所以它本质上是：

数据没有重建，但“每个节点住在哪个桶”重新安排了一遍。

11. 过期系统：现在采用“懒删除”

每个节点可以带：

has_expire
expire_at_ms

例如当前绝对时间：

100000 ms

执行：

EXPIRE foo 1

数据库保存的不是“1 秒”，而是：

expire_at_ms = 100000 + 1000 = 101000

之后查找时：

now_ms < 101000  → 仍有效
now_ms >= 101000 → 已过期

当前没有后台过期线程，也没有定期扫描。

所以：

foo 已经过期
   ↓
它可能暂时还物理存在于桶链里
   ↓
GET / EXISTS / TTL 等再次碰到它
   ↓
db_find_live_node 发现过期
   ↓
从链表摘掉
   ↓
free(key)
free(value)
free(node)
size--
   ↓
对上层表现为“不存在”

这就是 lazy expiration。

TTL 返回值

>= 0  → 剩余秒数
-1    → key 存在，但没有设置过期时间
-2    → key 不存在，或已过期并被视为不存在

12. DEL 微观逻辑：链表删除最容易错在哪里？

假设一个桶是：

bucket[10] ─► A ─► B ─► C ─► NULL

删除 A：

bucket[10] = A->next

变成：

bucket[10] ─► B ─► C

删除 B：

A->next = B->next

变成：

bucket[10] ─► A ─► C

之后必须完整释放：

free(node->key)
free(node->val)
free(node)
db->size--

所以删除不是一句“把 key 标没了”，而是一个真实的链表摘除 + 所有权回收过程。

13. 响应为什么还要分类型？

如果服务端所有东西都返回字符串：

"0"

客户端无法判断：

这是字符串值 "0"？
还是 DEL 删除了 0 个 key？
还是 TTL 为 0？

所以当前协议定义：

RESP_OK
RESP_ERR
RESP_STR
RESP_INT
RESP_NIL

响应 body 大致是：

OK / NIL
┌─────────────┐
│ 4-byte type │
└─────────────┘

STR / ERR
┌─────────────┬────────────┬─────────────┐
│ 4-byte type │ 4-byte len │ data bytes  │
└─────────────┴────────────┴─────────────┘

INT
┌─────────────┬──────────────────┐
│ 4-byte type │ 8-byte integer   │
└─────────────┴──────────────────┘

最外层还会再包：

[4-byte body_len][response body]

14. 一条 GET foo 的完整源码级旅行

如果前面已经：

SET foo bar

现在输入：

GET foo

运行路径是：

① tcp_client.c
   fgets() 读到 "GET foo"

② build_req()
   argc=2
   argv[0]="GET"
   argv[1]="foo"

③ proto.c / send_msg()
   发送 [body_len][request body]

④ server / poll()
   发现该 client fd 有 POLLIN

⑤ conn_on_readable()
   ST_READ_LEN：读满 4-byte length
   ST_READ_BODY：读满 request body

⑥ codec.c / parse_req()
   body → Args

⑦ tcp_server.c / do_request()
   argv[0] == GET
   → db_get(&g_db, "foo", ...)

⑧ db.c
   hash("foo")
   → % bucket_count
   → 找到目标桶
   → 沿 next 查节点
   → 检查是否过期
   → 找到 value="bar"

⑨ do_request()
   resp.type = RESP_STR
   resp.data = "bar"

⑩ codec.c / make_resp()
   Resp → [body_len][type][len][bar]

⑪ Conn
   wbuf = 完整响应
   wsent = 0
   state = ST_WRITE_RESP

⑫ poll()
   等 fd 出现 POLLOUT

⑬ conn_on_writable()
   能写多少写多少
   如果只写一部分，保存 wsent
   下轮继续

⑭ 写完
   free(wbuf)
   state = ST_READ_LEN
   等同一连接的下一条请求

⑮ client / recv_msg()
   收到完整 response body

⑯ parse_resp()
   得到 RESP_STR

⑰ print_resp()
   打印：
   < bar

这一条路径基本把项目所有核心层都串起来了。

15. 持久化：当前实现是快照，不是 AOF

当前持久化入口：

SAVE

保存文件：

my_redis.dump

文件格式：

[4-byte magic = "MYR1"]
[4-byte entry_count]

重复 entry_count 次：
    [4-byte key_len]
    [key bytes]
    [4-byte val_len]
    [value bytes]
    [4-byte has_expire]
    如果有过期时间：
        [8-byte expire_at_ms]

SAVE

flowchart TD
    S[SAVE] --> O[打开 my_redis.dump: wb]
    O --> M[写 magic MYR1]
    M --> C[统计仍有效的 entry 数量]
    C --> W[遍历所有 bucket / node]
    W --> X{节点已过期?}
    X -->|是| SKIP[跳过]
    X -->|否| DATA[写 key/value/expire metadata]
    DATA --> W
    SKIP --> W
    W --> END[关闭文件]

保存时不会把已经过期的数据写进去。

服务启动恢复

服务端启动会调用：

persist_load(&g_db, "my_redis.dump")

恢复没有直接往 g_db 一边读一边塞，而是：

文件
  ↓
校验 magic == MYR1
  ↓
创建临时数据库 tmp
  ↓
一条条读取
  ↓
跳过已经过期的记录
  ↓
全部成功？
  ├── 否 → 丢弃 tmp，原 db 尽量不受污染
  └── 是 → free 原 db，再用 tmp 替换

这个设计避免了：

坏快照读到一半
→ 主数据库只恢复了一半
→ 形成半新半旧状态

16. 项目目录和每个文件的职责

mini-redis-c/
├── Makefile
├── README.md
│
├── include/
│   ├── codec.h       请求 Args、RespType、Resp / RespView、编解码接口
│   ├── db.h          Db / DbNode、数据库操作接口
│   ├── nb.h          set_nonblocking
│   ├── persist.h     SAVE / load 接口、快照文件名
│   └── proto.h       最外层消息收发 [length][payload]
│
├── src/
│   ├── tcp_server.c  服务端主入口、poll、Conn 状态机、命令分发
│   ├── tcp_client.c  CLI 客户端、命令切分、结果打印
│   ├── codec.c       request / response payload 编解码
│   ├── proto.c       阻塞式 read_full/write_all/send_msg/recv_msg（客户端使用）
│   ├── db.c          哈希表、链表、rehash、删除、EXPIRE / TTL
│   ├── persist.c     快照保存和启动加载
│   └── nb.c          fcntl + O_NONBLOCK
│
├── docs/
│   ├── ch00_notes.md ... ch08_notes.md
│   └── runtime_walkthrough.md
│
└── archive/
    └── 各章节早期版本 / 开发计划，用于保留演进轨迹

当前真正的运行主线是：

client
  ↓
tcp + custom protocol
  ↓
tcp_server.c
  ↓
codec.c
  ↓
do_request
  ↓
db.c / persist.c
  ↓
Resp
  ↓
codec.c
  ↓
client

17. 从最初版本到现在，项目是怎么长出来的？

ch00  整理项目结构 / docs / archive
  ↓
ch01  把 KV 存储从 server 拆成独立 db.*
  ↓
ch02  线性存储 → 哈希桶 + 链表
  ↓
ch03  加入扩容 / rehash
  ↓
ch04  加入 DEL
  ↓
ch05  加入 EXISTS
  ↓
ch06  响应语义升级为 OK / ERR / STR / INT / NIL
  ↓
ch07  加入 EXPIRE / TTL + 懒删除
  ↓
ch08  加入 SAVE + 启动加载快照

archive/ 不是运行代码，而是刻意保留的旧阶段切片。

它能让人看到设计是怎样一步步从：

“TCP 能收发”

演进成：

“有网络层、协议层、命令层、哈希存储、过期语义、快照恢复的最小数据库服务”

18. 编译与运行

环境：Linux / WSL + GCC + Make。

make

生成：

./server
./client

清理：

make clean

终端 1：

./server

终端 2：

./client

示例：

> SET foo bar
< OK

> GET foo
< bar

> EXISTS foo
< 1

> EXPIRE foo 2
< 1

> TTL foo
< 2

# 等待超过 2 秒
> GET foo
< (nil)

> SET name chuan
< OK

> SAVE
< OK

重新启动 server 后，如果 my_redis.dump 中仍有有效数据，会在启动阶段恢复。

19. 当前实现的真实边界

这个仓库刻意区分“已经实现”和“计划实现”。

目前还没有：

Redis 官方 RESP 兼容

redis-cli 直接连接兼容

AOF append-only log

后台自动 snapshot

主从复制 / Sentinel / Cluster

多线程命令执行

epoll

主动定期过期扫描

List / Set / Hash / ZSet 等 Redis 数据类型

完整引号/转义 CLI 解析（当前 client 只按空格 strtok）

生产级 crash consistency / checksum / fsync 策略

另外，当前 tcp_server.c 还保留了一段旧阶段的 PING/ECHO 风格函数 conn_make_resp，用于展示演进痕迹；它不在当前主执行路径中。

当前最准确的定位是：

一个以学习数据库和服务端底层机制为目标的单线程、poll 驱动、Redis-like 字符串 KV 服务。

20. 下一步可以往哪里继续？

按当前结构，比较自然的路线是：

① 增加自动化测试
② 清理旧主线代码 / 降低 tcp_server.c 职责
③ 主动过期扫描
④ 把当前协议演进为真正 RESP2 / RESP3 子集
⑤ 增加 AOF + replay
⑥ 增加 append / fsync 策略
⑦ benchmark：吞吐、P50/P95、不同 key 数下 rehash 影响
⑧ epoll / 更完整事件循环
⑨ 更多数据类型
⑩ 再考虑线程、分片、高可用

如果继续做 AOF，建议保持和当前 snapshot 分开理解：

当前 SAVE snapshot
= 某个时间点把“现在的数据库状态”拍一张照片

未来 AOF
= 每次写命令都追加一条操作日志，重启时通过 replay 重建状态

21. 源码阅读顺序

第一次看代码，建议不要按目录字母顺序看，而是沿一次请求走：

1. src/tcp_server.c
   main / Conn / poll

2. src/tcp_client.c
   build_req

3. include/proto.h + src/proto.c
   消息边界

4. include/codec.h + src/codec.c
   Args / Resp / 编解码

5. tcp_server.c
   do_request

6. include/db.h + src/db.c
   哈希表 / rehash / expire

7. include/persist.h + src/persist.c
   SAVE / load

8. docs/runtime_walkthrough.md
   再把整条链串一次

更完整的逐函数运行轨迹已经放在：

docs/runtime_walkthrough.md

docs/ch00_notes.md ~ docs/ch08_notes.md

archive/：历史阶段代码与计划记录

22. 一句话总结

终端命令
  → 自定义二进制协议
  → TCP
  → poll + 非阻塞 Conn 状态机
  → Args
  → 命令分发
  → 哈希表 KV / TTL / Snapshot
  → Resp
  → 网络响应
  → client 输出

这个项目的重点不是“实现了多少 Redis 命令”，而是亲手把一个网络数据库最核心的几层真正连接起来：

连接如何被管理、TCP 字节如何组成消息、请求如何被解析、数据如何在内存中组织、过期如何影响查询、状态如何落盘，以及结果最后如何回到客户端。
