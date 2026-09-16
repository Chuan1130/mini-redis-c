# runtime_walkthrough

这份文档是当前项目的“运行轨迹式源码导读”。

它不是按文件目录顺序介绍代码，而是尽量按照程序真实运行顺序来讲：

1. 服务端先怎么启动。
2. 客户端怎么把你输入的命令变成字节。
3. 服务端怎么读到这些字节。
4. 服务端怎么解析、执行、访问数据库、持久化。
5. 服务端怎么把结果编码并写回。
6. 客户端怎么把结果读出来并打印给你看。

阅读时建议打开源码一起看。你可以先打开 `src/tcp_server.c`，从 `main` 函数开始跟着本文走。

## 0. 当前项目处于哪个阶段

当前项目大致处于 `ch08`，也就是“核心主线收束章”。

到这一章为止，项目已经不是只有一个能回显文本的 TCP demo，而是有了一个最小 Redis 风格闭环：

- 网络层：服务端可以监听 TCP 端口，客户端可以连接服务端。
- 事件循环：服务端使用 `poll` 管理多个客户端连接。
- 协议层：请求和响应都有明确的二进制格式。
- 命令层：服务端可以识别 `SET`、`GET`、`DEL`、`EXISTS`、`EXPIRE`、`TTL`、`SAVE`。
- 数据库层：内存里用哈希表保存 key-value，并支持扩容、删除、存在性检查和过期时间。
- 持久化层：`SAVE` 可以把数据库保存到本地文件，服务端启动时可以尝试恢复。

如果把前面几章串起来看，大致是这样演进的：

```text
ch00：整理项目结构，补 README/docs/archive
ch01：把内存 KV 存储层从 tcp_server.c 里拆成 db.*
ch02：把数组查找升级成哈希桶 + 链表
ch03：给哈希表加入最小 rehash 能力
ch04：加入 DEL 删除命令
ch05：加入 EXISTS 存在性检查
ch06：把响应语义整理成 OK / ERR / STR / INT / NIL
ch07：加入 EXPIRE / TTL，支持最小过期时间
ch08：加入 SAVE 和启动加载，支持最小快照持久化
```

本文件只讲当前主线实现。`archive/` 里的旧实现只作为对照学习资料，不是当前运行路径的重点。

## 1. 项目整体地图

当前真正参与主线运行的文件主要是这些：

```text
src/tcp_server.c       服务端主程序：监听、连接、读请求、执行命令、写响应
src/tcp_client.c       客户端主程序：读取终端输入、编码请求、收响应、打印结果

include/codec.h
src/codec.c            payload 内部编解码：解析请求参数、编码/解析响应类型

include/db.h
src/db.c               内存数据库：哈希表、key-value、删除、存在性、过期时间

include/persist.h
src/persist.c          快照持久化：SAVE 保存、服务端启动时加载

include/proto.h
src/proto.c            客户端使用的外层消息收发：[4字节长度][payload]

include/nb.h
src/nb.c               服务端使用的非阻塞设置：set_nonblocking
```

注意一个容易混淆的点：

- 客户端使用 `proto.c` 里的 `send_msg` / `recv_msg` 来发送和接收整条消息。
- 服务端没有直接用 `recv_msg`，而是在 `tcp_server.c` 的 `Conn` 状态机里自己读 4 字节长度和 body。

为什么服务端要自己读？

因为服务端是非阻塞、多连接的 `poll` 模型。一次 `read` 可能只读到一半请求，不能像阻塞式客户端那样一直等到读完整条消息，否则一个客户端会卡住整个服务端。所以服务端要在 `Conn` 结构体里保存“这条连接现在读到哪里了”。

最核心的数据流可以先用这个图记住：

```text
你在客户端输入命令
        |
        v
src/tcp_client.c: build_req
把一行文本拆成 argc/argv，并编码成 request payload
        |
        v
src/proto.c: send_msg
在 payload 前加 4 字节长度，通过 TCP 发给服务端
        |
        v
src/tcp_server.c: conn_on_readable
非阻塞读取 4 字节长度，再读取完整 body
        |
        v
src/codec.c: parse_req
把 body 解析成 Args：argc、argv[i]、lens[i]
        |
        v
src/tcp_server.c: do_request
根据 argv[0] 分发 SET / GET / DEL / EXISTS / EXPIRE / TTL / SAVE
        |
        +--------------------+
        |                    |
        v                    v
src/db.c                 src/persist.c
访问内存哈希表            SAVE 时保存快照
        |                    |
        +---------+----------+
                  |
                  v
src/tcp_server.c: Resp
把执行结果表达成 OK / ERR / STR / INT / NIL
                  |
                  v
src/codec.c: make_resp
把 Resp 编码成响应字节：[长度][type][内容]
                  |
                  v
src/tcp_server.c: conn_on_writable
非阻塞写回客户端
                  |
                  v
src/proto.c: recv_msg
客户端读到完整响应 payload
                  |
                  v
src/codec.c: parse_resp
客户端把 payload 解析成 RespView
                  |
                  v
src/tcp_client.c: print_resp
打印 < OK、< bar、< (nil)、< 1 等结果
```

## 2. 程序启动总览

这个项目有两个可执行文件：

```text
./server
./client
```

学习时建议先看服务端，再看客户端。

原因是：

- 服务端包含当前项目最完整的主线：网络、状态机、命令层、数据库、持久化。
- 客户端主要是帮助你输入命令、编码请求、打印响应。

服务端入口是：

```text
src/tcp_server.c 的 int main(void)
```

客户端入口是：

```text
src/tcp_client.c 的 int main(void)
```

实际运行顺序通常是：

```bash
make
./server
./client
```

`./server` 会先启动并监听端口 `15001`。

`./client` 会连接本机的 `127.0.0.1:15001`，然后等待你在终端输入命令。

## 3. 服务端运行轨迹：从 main 开始

这一节是重点。请打开 `src/tcp_server.c`，找到 `int main(void)`。

### 3.1 创建监听 socket

服务端一开始执行：

```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
if (fd < 0) die("socket");
```

这里的名字含义：

- `fd` 是 file descriptor 的缩写，中文可以理解成“文件描述符”。
- 在 Unix/Linux 里，socket 也像文件一样用一个整数编号表示。
- `AF_INET` 表示使用 IPv4。
- `SOCK_STREAM` 表示使用 TCP 这种可靠字节流协议。
- 第三个参数 `0` 表示让系统根据前两个参数选择默认协议，也就是 TCP。

这一步的输入是：协议族和 socket 类型。

这一步的输出是：一个监听 socket 的文件描述符，保存在 `fd` 里。

如果 `socket` 返回负数，说明创建失败。此时会调用：

```c
die("socket");
```

`die` 在同一个文件里定义，它会调用 `perror(msg)` 打印系统错误，然后 `exit(1)` 退出程序。

小结：这一小段向操作系统申请了一个“以后用来监听客户端连接的入口”。

### 3.2 设置 SO_REUSEADDR

接着是：

```c
int val = 1;
if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
    die("setsockopt");
```

变量名解释：

- `val` 是 value 的缩写，这里表示“打开这个选项”。
- `SO_REUSEADDR` 大致意思是 reuse address，也就是“允许复用地址”。

为什么需要它？

服务端退出后，旧 TCP 连接可能还处在 `TIME_WAIT` 状态。如果不设置这个选项，你马上重新启动服务端时，`bind` 同一个端口可能失败。

对初学者来说，可以先记住：

```text
SO_REUSEADDR 让开发调试时重启服务端更顺畅。
```

小结：这一段不是业务逻辑，而是让服务端更容易重复启动。

### 3.3 准备服务端地址

然后是：

```c
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family = AF_INET;
addr.sin_port = htons(15001);
addr.sin_addr.s_addr = htonl(INADDR_ANY);
```

这里的 `sockaddr_in` 是 IPv4 socket 地址结构体。

字段解释：

- `addr`：address 的缩写，表示服务端要绑定到哪个地址。
- `sin_family`：地址族，设置成 `AF_INET`，表示 IPv4。
- `sin_port`：端口号，这里是 `15001`。
- `sin_addr.s_addr`：IP 地址，这里是 `INADDR_ANY`，表示监听本机所有网卡。

为什么先 `memset`？

`struct sockaddr_in` 里有些字段当前不一定手动赋值。如果不清零，里面可能是随机垃圾值。清零后再填关键字段更安全。

为什么端口要 `htons(15001)`？

- `htons` 可以理解成 host to network short。
- host 表示本机字节序。
- network 表示网络字节序。
- short 表示 16 位整数。

网络上传输多字节整数时，一般使用网络字节序。端口是 16 位整数，所以用 `htons`。

为什么 IP 要 `htonl(INADDR_ANY)`？

- `htonl` 可以理解成 host to network long。
- long 这里表示 32 位整数。
- IPv4 地址是 32 位，所以用 `htonl`。

小结：这一小段准备好“我要监听哪个 IP 和端口”。

### 3.4 bind 和 listen

下一步是：

```c
if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    die("bind");

if (listen(fd, SOMAXCONN) < 0)
    die("listen");
```

`bind` 的作用：

```text
把 socket fd 和本地地址 0.0.0.0:15001 绑定起来。
```

也就是说，操作系统以后收到发往这个端口的 TCP 连接请求，就知道应该交给这个 socket。

`listen` 的作用：

```text
把这个 socket 从“普通 socket”变成“监听 socket”。
```

`SOMAXCONN` 是系统允许的一个连接排队上限。这里先用系统默认的较大值，不在当前章节展开复杂调优。

小结：到这里，服务端已经正式开始监听 `15001` 端口。

### 3.5 初始化连接槽数组

接着是：

```c
Conn conns[MAX_CONN];
for (int i = 0; i < MAX_CONN; i++) conns[i].fd = -1;
```

这里出现了两个重要名字：

- `MAX_CONN`：最大连接数，目前定义为 `1024`。
- `Conn`：connection 的缩写，表示一个客户端连接的状态。

`Conn` 在 `src/tcp_server.c` 里定义：

```c
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
```

逐个字段解释：

- `fd`：这个客户端连接对应的 socket 文件描述符。
- `st`：state 的缩写，表示连接当前处于哪个阶段。
- `lenbuf[4]`：保存请求开头 4 字节长度字段。
- `len_got`：length got 的意思，表示 4 字节长度已经读到了几个字节。
- `body_len`：请求 body，也就是 payload 的总长度。
- `body`：指向堆上申请的请求 body 缓冲区。
- `body_got`：body got 的意思，表示 body 已经读到了多少字节。
- `wbuf`：write buffer 的缩写，表示待写回客户端的响应缓冲区。
- `wlen`：write length 的缩写，表示响应总长度。
- `wsent`：write sent 的缩写，表示响应已经写出去多少字节。

`ConnState` 是连接状态枚举：

```c
typedef enum {
    ST_READ_LEN,
    ST_READ_BODY,
    ST_WRITE_RESP
} ConnState;
```

三个状态的含义：

- `ST_READ_LEN`：正在读取请求开头的 4 字节长度。
- `ST_READ_BODY`：已经知道 body 长度，正在读取完整 body。
- `ST_WRITE_RESP`：请求已经处理完，正在把响应写回客户端。

为什么需要这些状态？

因为服务端使用非阻塞 I/O。一次 `read` 不保证读完整条消息，一次 `write` 也不保证写完整条响应。服务端必须记住“上次处理到哪里了”，下次 socket 再可读或可写时继续。

为什么 `fd` 初始设成 `-1`？

因为合法文件描述符通常是非负整数。用 `-1` 表示这个槽位空着，还没有客户端连接。

小结：这一小段准备了最多 1024 个“连接状态格子”，后面每接入一个客户端，就放进一个空格子。

### 3.6 初始化全局数据库 g_db

服务端文件顶部有：

```c
static Db g_db;
```

`g_db` 可以读成 global database，意思是“全局数据库”。

为什么是全局的？

当前项目是单进程、单线程教学版本。所有客户端共享同一份内存数据库，所以用一个全局 `Db` 对象最直接。

`Db` 在 `include/db.h` 里定义：

```c
typedef struct {
    DbNode *buckets[DB_MAX_BUCKET_COUNT];
    size_t bucket_count;
    size_t size;
} Db;
```

字段解释：

- `buckets`：bucket 是“桶”的意思。这里是哈希表的桶数组，每个元素是某条链表的头指针。
- `bucket_count`：当前启用了多少个桶。注意不是数组最大长度，而是当前活跃桶数。
- `size`：当前数据库里有多少个 key-value。

相关常量：

```c
#define DB_MAX_KV 1024
#define DB_INITIAL_BUCKET_COUNT 256
#define DB_MAX_BUCKET_COUNT 2048
```

含义：

- `DB_MAX_KV`：最多保存 1024 个键值对。
- `DB_INITIAL_BUCKET_COUNT`：数据库刚开始使用 256 个桶。
- `DB_MAX_BUCKET_COUNT`：最多可以扩到 2048 个桶。

服务端启动时执行：

```c
db_init(&g_db);
```

现在跳到 `src/db.c` 看 `db_init`：

```c
void db_init(Db *db)
{
    memset(db, 0, sizeof(*db));
    db->bucket_count = DB_INITIAL_BUCKET_COUNT;
}
```

这里的输入是：`&g_db`，也就是全局数据库对象的地址。

这里的输出是：`g_db` 被设置成空数据库。

执行后：

- 所有桶头指针都是 `NULL`。
- `size` 是 `0`。
- `bucket_count` 是 `256`。

为什么 `memset` 后还要设置 `bucket_count`？

因为“桶里没有数据”和“当前启用多少桶”是两件事。清零只能把指针和 size 清成 0，但数据库仍然需要知道：一开始应该按 256 个桶来计算 key 的位置。

小结：服务端现在有了一份空的内存数据库。

### 3.7 启动时尝试加载快照

接下来服务端执行：

```c
int load_rc = persist_load(&g_db, SNAPSHOT_FILE);
if (load_rc < 0) {
    fprintf(stderr, "warning: failed to load snapshot: %s\n", SNAPSHOT_FILE);
}
```

`SNAPSHOT_FILE` 在 `include/persist.h` 里定义：

```c
#define SNAPSHOT_FILE "my_redis.dump"
```

所以服务端会尝试从项目目录下的 `my_redis.dump` 加载历史数据。

`persist_load` 在 `src/persist.c` 里实现。它的返回值语义是：

- `0`：加载成功。
- `1`：文件不存在，不算错误，说明没有历史快照。
- `-1`：文件存在但加载失败，比如文件损坏。

为什么文件不存在不算错误？

第一次启动服务端时，本来就还没有保存过快照。此时应该用空数据库正常启动，而不是报错退出。

`persist_load` 的大致流程是：

```text
1. fopen(path, "rb") 打开快照文件
2. 读取 4 字节魔数，确认文件格式是 MYR1
3. 读取 entry 数量 count
4. 初始化一个临时数据库 tmp
5. 一条条读 key/value/过期时间
6. 如果记录没有过期，就写进 tmp
7. 全部成功后，释放旧 db，把 tmp 赋给 db
```

这里有一个很重要的设计：先加载到临时数据库 `tmp`。

为什么不直接写进 `g_db`？

假设快照文件坏了一半，如果直接把前半部分写进 `g_db`，读到后面失败时，主数据库就会变成“半新半旧”的混乱状态。先加载到 `tmp`，只有全部成功才替换主数据库，会更安全。

小结：服务端启动时会尽量恢复上次 `SAVE` 保存的数据库状态；如果没有快照，就从空库开始。

### 3.8 设置监听 socket 为非阻塞

接着服务端执行：

```c
set_nonblocking(fd);
```

这个函数声明在 `include/nb.h`，实现在 `src/nb.c`：

```c
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```

名字解释：

- `nb` 可以理解成 non-blocking 的缩写。
- `flags` 表示这个文件描述符原来的状态标志。
- `O_NONBLOCK` 表示打开非阻塞模式。

阻塞和非阻塞有什么区别？

阻塞模式下，如果没有新连接，`accept` 可能一直卡住；如果没有数据，`read` 可能一直卡住。

非阻塞模式下，如果暂时没有数据，系统调用会返回错误码 `EAGAIN` 或 `EWOULDBLOCK`，意思是“现在还不能做，稍后再试”。

当前服务端使用 `poll`。`poll` 会告诉我们哪些 socket 已经可读或可写。只有配合非阻塞 I/O，事件循环才不会被某一个连接拖住。

小结：监听 socket 切换成非阻塞模式，为后面的 `poll` 循环做准备。

### 3.9 进入 poll 事件循环

服务端后面进入：

```c
while (1) {
    ...
    int r = poll(pfds, nfd, -1);
    ...
}
```

这是服务端长期运行的核心循环。

`poll` 的作用可以理解成：

```text
请操作系统帮我等一等：
哪些 socket 有新连接？
哪些 socket 有数据可读？
哪些 socket 可以继续写？
```

每一轮循环都会先准备两个数组：

```c
struct pollfd pfds[MAX_CONN + 1];
int idx_map[MAX_CONN + 1];
int nfd = 0;
```

名字解释：

- `pfds`：poll fds，交给 `poll` 的文件描述符数组。
- `idx_map`：index map，下标映射表，用来从 `pfds` 的下标找回 `conns` 的下标。
- `nfd`：number of fds，当前放进 `pfds` 的文件描述符数量。

第 0 个位置固定放监听 socket：

```c
pfds[nfd].fd = fd;
pfds[nfd].events = POLLIN;
idx_map[nfd] = -1;
nfd++;
```

这里 `POLLIN` 表示“我关心它什么时候可读”。

对监听 socket 来说，“可读”通常表示有新客户端连接可以 `accept`。

然后服务端遍历所有已有连接：

```c
for (int i = 0; i < MAX_CONN; i++) {
    if (conns[i].fd < 0) continue;
    pfds[nfd].fd = conns[i].fd;
    pfds[nfd].events = (conns[i].st == ST_WRITE_RESP) ? POLLOUT : POLLIN;
    idx_map[nfd] = i;
    nfd++;
}
```

这里有一个状态机思想：

- 如果连接处于 `ST_WRITE_RESP`，说明有响应要写回，所以关心 `POLLOUT`。
- 否则说明还在读请求阶段，所以关心 `POLLIN`。

小结：每一轮循环，服务端都会告诉操作系统：“这些 fd 是我关心的，请告诉我谁准备好了。”

### 3.10 接收新连接 accept

`poll` 返回后，服务端先看第 0 个监听 socket：

```c
if (pfds[0].revents & POLLIN) {
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        ...
    }
}
```

名字解释：

- `revents`：returned events，表示 `poll` 返回的实际事件。
- `cfd`：client fd，表示新客户端连接的文件描述符。

为什么这里有一个 `for (;;)`？

因为监听 socket 是非阻塞的。一次 `poll` 告诉我们有新连接时，可能已经排队了多个连接。这里会一直 `accept`，直到 `accept` 返回 `EAGAIN` 或 `EWOULDBLOCK`，表示暂时没有更多连接。

新连接接进来后：

```c
set_nonblocking(cfd);
```

客户端连接也要设成非阻塞。

然后找一个空槽位：

```c
for (int i = 0; i < MAX_CONN; i++) {
    if (conns[i].fd < 0) {
        conns[i].fd = cfd;
        conns[i].st = ST_READ_LEN;
        ...
        placed = 1;
        break;
    }
}
```

初始化后的状态是：

- `fd = cfd`：这个槽位属于新客户端。
- `st = ST_READ_LEN`：刚连接进来，下一步先读 4 字节长度。
- `len_got = 0`：长度还没读。
- `body_len = 0`：还不知道 body 多长。
- `body = NULL`：还没分配 body 缓冲区。
- `body_got = 0`：body 还没读。
- `wbuf = NULL`：还没有响应要写。
- `wlen = 0`：响应长度是 0。
- `wsent = 0`：响应还没写。

如果连接槽满了：

```c
if (!placed) close(cfd);
```

也就是直接关闭新连接。

小结：这一段把新客户端接进来，并为它准备一份 `Conn` 会话状态。

### 3.11 处理已有连接的读写事件

接下来服务端处理 `pfds[1]` 到 `pfds[nfd - 1]`：

```c
for (int k = 1; k < nfd; k++) {
    int i = idx_map[k];
    Conn *c = &conns[i];
    ...
}
```

为什么 `k` 从 1 开始？

因为 `pfds[0]` 是监听 socket，已经用于接收新连接了。后面的才是已连接客户端。

`idx_map[k]` 的作用是把 `pfds` 下标转回 `conns` 下标。

例如：

```text
pfds[3] 对应 conns[17]
那么 idx_map[3] = 17
```

这样服务端就能找到这个客户端连接自己的 `Conn` 状态。

如果连接出现错误：

```c
if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
    dead = 1;
}
```

几个事件含义：

- `POLLERR`：连接出错。
- `POLLHUP`：对端挂断。
- `POLLNVAL`：fd 无效。

如果连接可读，并且当前不是写响应阶段：

```c
int rr = conn_on_readable(c);
```

如果连接可写，并且当前正是写响应阶段：

```c
int ww = conn_on_writable(c);
```

如果处理后发现连接应该关闭：

```c
if (dead) conn_free(c);
```

`conn_free` 会：

- `close(c->fd)` 关闭 socket。
- `free(c->body)` 释放请求 body 缓冲区。
- `free(c->wbuf)` 释放响应缓冲区。
- `memset` 清空结构体。
- 把 `fd` 设回 `-1`，表示槽位空闲。

小结：这一段是事件分发中心，根据当前事件和连接状态决定读请求、写响应，还是关闭连接。

## 4. 服务端读取请求：conn_on_readable

当某个客户端 socket 可读时，服务端进入 `src/tcp_server.c` 的：

```c
static int conn_on_readable(Conn *c)
```

参数 `c` 是当前客户端连接的状态。

返回值含义：

- `0`：处理完成，连接还活着。
- `1`：对端关闭，应该回收连接。
- `-1`：发生错误，应该回收连接。

这个函数内部按 `ConnState` 分两段读：

```text
ST_READ_LEN   先读 4 字节长度
ST_READ_BODY  再读完整请求 body
```

### 4.1 读取 4 字节长度

当 `c->st == ST_READ_LEN` 时：

```c
ssize_t r = read(c->fd, c->lenbuf + c->len_got, 4 - c->len_got);
```

这里的意思是：

```text
从 socket 里读数据，
放到 lenbuf 还没填满的位置，
最多读取还缺的字节数。
```

例如：

- 一开始 `len_got = 0`，最多读 4 字节。
- 如果第一次只读到 2 字节，下次就从 `lenbuf + 2` 开始，再读剩下 2 字节。

为什么不假设一次就能读满 4 字节？

TCP 是字节流，不是消息流。即使客户端一次发送了完整消息，服务端一次 `read` 也可能只拿到一部分。

读完后：

```c
c->len_got += (size_t)r;
```

如果长度刚好读满 4 字节：

```c
uint32_t net = 0;
memcpy(&net, c->lenbuf, 4);
c->body_len = ntohl(net);
```

这里：

- `net` 表示网络字节序的长度。
- `ntohl` 可以理解成 network to host long，把 32 位整数从网络字节序转成本机字节序。
- `body_len` 就是请求 payload 的长度。

接着检查：

```c
if (c->body_len > MAX_MSG_SIZE) return -1;
```

`MAX_MSG_SIZE` 是最大消息体长度，目前是 1MB。这个检查可以防止客户端发一个超大长度导致服务端乱分配内存。

然后分配 body 缓冲区：

```c
c->body = (uint8_t*)malloc(c->body_len + 1);
if (!c->body) return -1;
c->body_got = 0;
c->st = ST_READ_BODY;
```

为什么 `+ 1`？

多出来的 1 字节在某些调试场景可以放 `'\0'`，方便把内容当字符串观察。不过真正有效长度仍然是 `body_len`。

小结：这一段读到了“后面 payload 有多长”，并准备好一块内存来接收 payload。

### 4.2 读取请求 body

当 `c->st == ST_READ_BODY` 时：

```c
ssize_t r = read(c->fd, c->body + c->body_got, c->body_len - c->body_got);
```

和读长度类似，这里也是从还没填满的位置继续读。

字段含义：

- `body`：整条请求 payload 的缓冲区。
- `body_len`：payload 应该有多长。
- `body_got`：目前已经读了多少。

当：

```c
c->body_got == c->body_len
```

说明一整条请求 body 读完了。

这时服务端终于可以解析请求。

小结：这一段把一条完整请求 payload 从 TCP 字节流里取出来。

### 4.3 解析请求 parse_req

请求读完整后，代码进入：

```c
Args args;

if (parse_req(c->body, c->body_len, &args) != 0) {
    ...
} else {
    ...
}
```

`Args` 在 `include/codec.h` 里定义：

```c
typedef struct {
    uint32_t argc;
    const uint8_t *argv[MAX_ARGS];
    uint32_t lens[MAX_ARGS];
} Args;
```

名字解释：

- `argc`：argument count，参数个数。
- `argv`：argument vector，参数数组。这里每个元素是指向原始请求缓冲区的一段字节。
- `lens`：lengths，每个参数的长度。
- `MAX_ARGS`：最多 16 个参数。

重点：`argv[i]` 不是新复制的字符串，它只是指向 `c->body` 里面的一段。

例如客户端输入：

```text
SET foo bar
```

请求 payload 会长得像这样：

```text
[argc=3]
[len=3]["SET"]
[len=3]["foo"]
[len=3]["bar"]
```

解析后：

```text
args.argc = 3
args.argv[0] -> "SET", args.lens[0] = 3
args.argv[1] -> "foo", args.lens[1] = 3
args.argv[2] -> "bar", args.lens[2] = 3
```

现在跳到 `src/codec.c` 看 `parse_req`。

`parse_req` 的核心流程：

```text
1. cur 指向 data 开头
2. end 指向 data + size
3. 先 read_u32 读 argc
4. 检查 argc 不能是 0，也不能超过 MAX_ARGS
5. 循环读取每个参数：
   - read_u32 读参数长度 len
   - read_bytes 读 len 个字节
   - 把这段字节的起始指针和长度放进 Args
6. 最后检查 cur == end，确保没有多余字节
```

`read_u32` 的作用：

```c
bool read_u32(const uint8_t **cur,
              const uint8_t *end,
              uint32_t *out)
```

它从当前游标 `*cur` 处读 4 字节网络字节序整数，成功后把 `*cur` 向后移动 4 字节。

`read_bytes` 的作用：

```c
bool read_bytes(const uint8_t **cur,
                const uint8_t *end,
                uint32_t n,
                const uint8_t **out)
```

它从当前游标读 `n` 个字节，成功后把这段字节的起始地址放进 `out`，并推进游标。

为什么要有 `cur` 和 `end`？

这是为了做边界检查。每次读之前都要确认剩余字节足够，避免读出缓冲区范围。

小结：`parse_req` 把原始 payload 变成了命令层更容易理解的 `Args` 参数视图。

### 4.4 请求格式错误时返回 bad request

如果 `parse_req` 失败，服务端构造一个错误响应：

```c
Resp resp = {
    .type = RESP_ERR,
    .data = (const uint8_t *)"bad request",
    .len = 11,
    .integer = 0,
};
uint8_t *out = make_resp(&resp, &out_len);
```

`Resp` 在 `include/codec.h` 里定义：

```c
typedef struct {
    RespType type;
    const uint8_t *data;
    uint32_t len;
    int64_t integer;
} Resp;
```

字段解释：

- `type`：响应类型，比如 OK、ERR、STR、INT、NIL。
- `data`：当响应是字符串或错误文本时，指向那段字节。
- `len`：`data` 的长度。
- `integer`：当响应是整数时，保存整数值。

这里 `RESP_ERR` 表示错误，`data` 是错误文本 `"bad request"`。

注意：`Resp` 还不是网络字节。它只是服务端内部的“响应语义对象”。真正编码成可发送字节的是 `make_resp`。

小结：如果请求 payload 格式不合法，服务端不会进入数据库层，而是直接返回协议错误。

### 4.5 请求解析成功后进入 do_request

如果 `parse_req` 成功：

```c
Resp resp;

if (do_request(&args, &resp) != 0) {
    resp.type = RESP_ERR;
    resp.data = (const uint8_t *)"internal error";
    resp.len = 14;
    resp.integer = 0;
}
```

`do_request` 在 `src/tcp_server.c` 里定义，它是当前项目的命令分发层。

输入：

- `args`：已经解析好的命令参数。

输出：

- `resp`：本次命令应该返回给客户端的结果。

小结：从这里开始，程序从“协议解析”进入“业务命令执行”。

## 5. 服务端命令分发：do_request

`do_request` 的第一步：

```c
if (args->argc == 0) return -1;
```

如果没有参数，就没有命令名，无法执行。

然后取出命令名：

```c
const char *cmd = (const char *)args->argv[0];
uint32_t cmd_len = args->lens[0];
```

注意：`cmd` 指向的是请求 body 里的字节片段，不一定是标准 C 字符串。标准 C 字符串必须以 `'\0'` 结尾，但这里的参数只是“指针 + 长度”。

所以后面判断命令时总是同时检查长度，例如：

```c
cmd_len == 3 && strncasecmp(cmd, "set", 3) == 0
```

这样可以避免把 `"setxxx"` 误判成 `"set"`。

`strncasecmp` 中的 `n` 表示最多比较 n 个字符，`case` 表示大小写，整体意思是“忽略大小写比较前 n 个字符”。

然后给响应一个默认错误值：

```c
out->type = RESP_ERR;
out->data = NULL;
out->len = 0;
out->integer = 0;
```

这样即使后面某个分支忘记设置，`out` 也不是完全未初始化的随机状态。

### 5.1 SET key value

如果命令是 `SET`：

```c
if (cmd_len == 3 && strncasecmp(cmd, "set", 3) == 0) {
    if (args->argc != 3) goto err;

    DbStatus rc = db_set(&g_db,
                         args->argv[1],
                         args->lens[1],
                         args->argv[2],
                         args->lens[2]);
    ...
}
```

`SET key value` 必须有 3 个参数：

```text
argv[0] = 命令名 SET
argv[1] = key
argv[2] = value
```

例如：

```text
SET foo bar
```

会调用：

```text
db_set(&g_db, "foo", 3, "bar", 3)
```

这里跳到 `src/db.c` 的 `db_set`。

`db_set` 做的事情：

```text
1. 如果当前哈希表负载偏高，先 rehash 扩容
2. 根据 key 算桶下标
3. 在桶链里查找这个 key 是否已经存在且未过期
4. 如果存在：替换 value，并清除旧过期时间
5. 如果不存在：复制 key、复制 value、分配新 DbNode、头插到桶链表
```

为什么 `db_set` 必须复制 key 和 value？

因为 `args->argv[1]` 和 `args->argv[2]` 指向的是请求缓冲区 `c->body`。这条请求处理完后，`c->body` 会被释放。如果数据库直接保存这些指针，后面再 `GET` 时就会读到已经释放的内存。

所以 `db_set` 内部会用 `db_dup_bytes` 复制出数据库自己持有的副本。

如果 `db_set` 成功，`do_request` 设置：

```c
out->type = RESP_OK;
```

表示简单成功，不带额外数据。

小结：`SET` 的输入是 key 和 value，输出是 `RESP_OK`，真正的数据写入发生在 `db_set`。

### 5.2 GET key

如果命令是 `GET`：

```c
else if (cmd_len == 3 && strncasecmp(cmd, "get", 3) == 0) {
    if (args->argc != 2) goto err;

    DbStatus rc = db_get(&g_db,
                         args->argv[1],
                         args->lens[1],
                         &out->data,
                         &out->len);

    if (rc == DB_STATUS_OK) {
        out->type = RESP_STR;
        return 0;
    }

    out->type = RESP_NIL;
}
```

`GET key` 必须有 2 个参数：

```text
argv[0] = GET
argv[1] = key
```

它调用 `db_get`。

`db_get` 的输出参数是：

- `out->data`：指向数据库内部保存的 value。
- `out->len`：value 的长度。

如果找到 key，就返回 `RESP_STR`，表示字符串结果。

如果没找到 key，就返回 `RESP_NIL`，表示空结果。

为什么 `GET missing` 不是 `RESP_ERR`？

因为 key 不存在不是命令执行失败。它只是查询结果为空。真正的错误应该是命令拼错、参数数量不对、协议格式错这类情况。

小结：`GET` 会访问数据库，如果找到就返回字符串，找不到就返回 nil。

### 5.3 DEL key

如果命令是 `DEL`：

```c
else if (cmd_len == 3 && strncasecmp(cmd, "del", 3) == 0) {
    if (args->argc != 2) goto err;

    DbStatus rc = db_del(&g_db, args->argv[1], args->lens[1]);
    out->type = RESP_INT;
    out->integer = (rc == DB_STATUS_OK) ? 1 : 0;
}
```

`DEL key` 必须有 2 个参数。

它调用 `db_del` 删除 key。

返回：

- 删除到了：整数 `1`。
- 没有这个 key：整数 `0`。

为什么返回整数？

这更接近“影响了几个 key”的语义。删除到一个 key 就是 1，没删到就是 0。

小结：`DEL` 会修改数据库，结果用整数表达。

### 5.4 EXISTS key

如果命令是 `EXISTS`：

```c
else if (cmd_len == 6 && strncasecmp(cmd, "exists", 6) == 0) {
    if (args->argc != 2) goto err;

    DbStatus rc = db_exists(&g_db, args->argv[1], args->lens[1]);
    out->type = RESP_INT;
    if (rc == DB_STATUS_OK) {
        out->integer = 1;
    } else {
        out->integer = 0;
    }
}
```

`EXISTS key` 的问题是：

```text
这个 key 当前存在吗？
```

返回：

- 存在：整数 `1`。
- 不存在：整数 `0`。

它只关心 key 是否存在，不需要把 value 拿出来，所以调用 `db_exists`。

小结：`EXISTS` 是只问存在性，不返回 value。

### 5.5 EXPIRE key seconds

如果命令是 `EXPIRE`：

```c
else if (cmd_len == 6 && strncasecmp(cmd, "expire", 6) == 0) {
    if (args->argc != 3) goto err;

    int64_t seconds = 0;
    if (arg_to_i64(args->argv[2], args->lens[2], &seconds) != 0 || seconds < 0) {
        ...
    }
    ...
    DbStatus rc = db_expire(&g_db,
                            args->argv[1],
                            args->lens[1],
                            seconds * 1000);
    out->type = RESP_INT;
    out->integer = (rc == DB_STATUS_OK) ? 1 : 0;
}
```

`EXPIRE key seconds` 必须有 3 个参数：

```text
argv[0] = EXPIRE
argv[1] = key
argv[2] = seconds
```

例如：

```text
EXPIRE foo 1
```

表示让 `foo` 在 1 秒后过期。

这里先调用 `arg_to_i64` 把第三个参数解析成整数。

`arg_to_i64` 为什么要复制一份临时字符串？

因为 `args->argv[2]` 仍然只是“字节片段”，不一定以 `'\0'` 结尾。而 `strtoll` 这类 C 标准库函数需要标准 C 字符串，所以 `arg_to_i64` 会：

```text
1. malloc(len + 1)
2. memcpy 参数字节
3. 在末尾补 '\0'
4. 调用 strtoll 解析
5. 检查是不是完整合法整数
6. free 临时字符串
```

解析成功后，命令层把秒换成毫秒：

```c
seconds * 1000
```

因为数据库内部用毫秒保存时间。

然后调用 `db_expire`。

返回：

- key 存在并设置成功：整数 `1`。
- key 不存在：整数 `0`。

小结：`EXPIRE` 的输入是 key 和秒数，数据库内部会保存一个绝对过期时刻。

### 5.6 TTL key

如果命令是 `TTL`：

```c
else if (cmd_len == 3 && strncasecmp(cmd, "ttl", 3) == 0) {
    if (args->argc != 2) goto err;

    int64_t ttl_ms = 0;
    DbStatus rc = db_ttl(&g_db, args->argv[1], args->lens[1], &ttl_ms);
    out->type = RESP_INT;

    if (rc == DB_STATUS_OK) {
        out->integer = (ttl_ms + 999) / 1000;
    } else if (rc == DB_STATUS_NO_EXPIRE) {
        out->integer = -1;
    } else {
        out->integer = -2;
    }
}
```

`TTL key` 用来查询 key 还剩多少秒过期。

数据库层返回毫秒 `ttl_ms`。命令层把它换成秒：

```c
(ttl_ms + 999) / 1000
```

为什么要加 999？

这是向上取整。例如还剩 1 毫秒时，如果直接 `/ 1000` 会变成 0 秒。但 key 明明还没过期，所以这里显示 1 秒更直观。

返回语义：

- `>= 0`：还剩多少秒。
- `-1`：key 存在，但没有设置过期时间。
- `-2`：key 不存在，或者访问时发现已经过期。

小结：`TTL` 不改变正常未过期 key 的 value，但如果发现 key 已经过期，数据库层会把它懒删除。

### 5.7 SAVE

如果命令是 `SAVE`：

```c
else if (cmd_len == 4 && strncasecmp(cmd, "save", 4) == 0) {
    if (args->argc != 1) goto err;

    if (persist_save(&g_db, SNAPSHOT_FILE) != 0) {
        out->type = RESP_ERR;
        out->data = (const uint8_t *)"save failed";
        out->len = 11;
        return 0;
    }

    out->type = RESP_OK;
}
```

`SAVE` 不需要 key，也不需要 value。

它调用：

```c
persist_save(&g_db, "my_redis.dump")
```

这会把当前数据库里仍然有效的数据写到快照文件。

保存成功返回 `RESP_OK`。

保存失败返回 `RESP_ERR`，错误文本是 `"save failed"`。

小结：`SAVE` 是当前项目的最小持久化入口。

### 5.8 参数数量错误和未知命令

如果参数数量不对，会跳到：

```c
err:
    out->type = RESP_ERR;
    out->data = (const uint8_t *)"wrong number of arguments";
    out->len = 25;
    return 0;
```

如果命令名不认识：

```c
out->type = RESP_ERR;
out->data = (const uint8_t *)"unknown command";
out->len = 15;
```

小结：命令层把业务结果统一转换成 `Resp`，后面再由协议层编码。

## 6. 数据库层运行轨迹

这一节重点看 `include/db.h` 和 `src/db.c`。

### 6.1 DbNode：一条 key-value 记录

`DbNode` 在 `include/db.h`：

```c
typedef struct DbNode {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
    bool has_expire;
    int64_t expire_at_ms;
    struct DbNode *next;
} DbNode;
```

字段解释：

- `key`：key 的字节内容，例如 `"foo"`。
- `key_len`：key 长度，例如 `3`。
- `val`：value 的字节内容，例如 `"bar"`。
- `val_len`：value 长度，例如 `3`。
- `has_expire`：这个 key 有没有设置过期时间。
- `expire_at_ms`：如果有过期时间，它在哪个绝对毫秒时刻过期。
- `next`：如果多个 key 落到同一个桶，就用链表串起来，`next` 指向下一个节点。

为什么不只用 `char *key`，而要保存 `key_len`？

因为数据库操作更可靠的方式是“指针 + 长度”。如果只靠 `'\0'` 判断字符串结束，就更容易被二进制数据或非标准字符串干扰。即使当前客户端只输入普通文本，这个项目也已经在向更通用的协议方式靠近。

例如：

```text
key = "foo", key_len = 3
key = "foobar", key_len = 6
```

即使它们前三个字符都一样，长度不同，也不是同一个 key。

小结：`DbNode` 是数据库实际保存的一条记录。

### 6.2 Db：整个哈希表数据库

`Db` 在 `include/db.h`：

```c
typedef struct {
    DbNode *buckets[DB_MAX_BUCKET_COUNT];
    size_t bucket_count;
    size_t size;
} Db;
```

可以想象成：

```text
buckets[0]  -> NULL
buckets[1]  -> DbNode("foo") -> DbNode("bar") -> NULL
buckets[2]  -> NULL
...
```

`buckets` 数组里不直接放 key/value，而是放每个桶链表的头指针。

如果两个 key 算出来的桶下标一样，就发生哈希冲突。当前项目用链表解决冲突。

小结：`Db` 是“桶数组 + 链表节点”的组合。

### 6.3 哈希计算：db_hash_key 和 db_bucket_index_with_count

写入或查找 key 时，先调用：

```c
static uint32_t db_hash_key(const uint8_t *key, uint32_t key_len)
```

这个函数使用 FNV-1a 风格的写法，让 key 的每个字节都参与计算，最终得到一个 32 位整数。

然后调用：

```c
static size_t db_bucket_index_with_count(const uint8_t *key,
                                         uint32_t key_len,
                                         size_t bucket_count)
{
    return (size_t)(db_hash_key(key, key_len) % bucket_count);
}
```

为什么要 `% bucket_count`？

哈希值可能很大，但桶数量有限。取模后才能把哈希值映射到 `0` 到 `bucket_count - 1` 的范围。

例如当前有 256 个桶：

```text
hash("foo") = 一个很大的数字
hash("foo") % 256 = 某个 0..255 的桶下标
```

小结：哈希函数负责把任意 key 映射到一个桶下标。

### 6.4 查找有效节点：db_find_live_node

从 ch07 开始，数据库里最核心的查找函数是：

```c
static DbNode *db_find_live_node(Db *db,
                                 size_t bucket_idx,
                                 const uint8_t *key,
                                 uint32_t key_len,
                                 DbNode **out_prev)
```

名字解释：

- `find`：查找。
- `live`：活着的、仍然有效的。
- `node`：哈希表链表节点。

它不只是“找到 key”，还要判断“找到的 key 有没有过期”。

流程：

```text
1. 取得当前时间 now_ms
2. 从目标桶的链表头开始扫描
3. 用 db_key_equals 精确比较 key 长度和内容
4. 如果 key 不匹配，继续看 next
5. 如果 key 匹配，再判断是否过期
6. 如果过期，调用 db_unlink_and_free_node 懒删除，然后返回 NULL
7. 如果没过期，返回这个节点
```

为什么叫“懒删除”？

因为项目当前没有后台线程定期扫描所有 key。过期 key 只有在被访问时才会被发现并删除。

例如：

```text
SET foo bar
EXPIRE foo 1
等待 2 秒
```

这时 `foo` 可能还躺在哈希表里。但是当你执行：

```text
GET foo
```

`db_find_live_node` 会发现它已经过期，于是把它从链表中删除，然后告诉上层“没找到”。

`out_prev` 是什么？

删除链表节点时，需要知道当前节点前面那个节点是谁。`out_prev` 就是把前一个节点带出去。

如果删除的是桶头节点，`prev` 是 `NULL`。

如果删除的是中间节点，`prev->next` 要改成 `node->next`。

小结：`db_find_live_node` 是当前数据库层最关键的“查找 + 过期判断 + 懒删除”入口。

### 6.5 SET 的数据库路径：db_set

`db_set` 的流程更完整地看是：

```text
1. 判断插入前是否应该 rehash
2. 算 key 的桶下标
3. 查找这个 key 是否已有未过期节点
4. 如果有：复制新 value，替换旧 value，清除过期时间
5. 如果没有：检查容量，复制 key，复制 value，分配 DbNode，头插到桶链
```

#### 6.5.1 为什么插入前可能 rehash

函数：

```c
static int db_should_rehash_before_insert(const Db *db)
```

判断条件是：

```text
如果下一次插入后，元素个数会超过桶数的 3/4，就建议扩容。
```

为什么不等冲突很严重再扩容？

哈希表希望每个桶里的链表尽量短。链表越长，查找就越慢。提前扩容可以减少冲突。

#### 6.5.2 rehash 做了什么

`db_rehash` 会：

```text
1. 准备一个新的桶头数组 new_buckets
2. 遍历旧 buckets 中所有节点
3. 对每个节点用新的 bucket_count 重新算桶下标
4. 把节点挂到新桶链里
5. 清空旧桶数组
6. 把新桶头复制回 db->buckets
7. 更新 db->bucket_count
```

注意：rehash 不是重新创建所有 key/value。节点本身还在，只是换了桶位置和 `next` 链接关系。

#### 6.5.3 覆盖已有 key

如果 `db_find_live_node` 找到了未过期节点：

```c
uint8_t *new_val = db_dup_bytes(val, val_len);
free(node->val);
node->val = new_val;
node->val_len = val_len;
node->has_expire = false;
node->expire_at_ms = 0;
```

为什么覆盖时清除过期时间？

当前项目的语义是：重新 `SET` 一个 key，相当于给它写入一个新值，并让它回到没有过期时间的状态。

例如：

```text
SET foo old
EXPIRE foo 10
SET foo new
TTL foo
```

此时 `TTL foo` 会返回 `-1`，表示 key 存在但没有过期时间。

#### 6.5.4 新增 key

如果 key 不存在：

```text
1. db_dup_bytes 复制 key
2. db_dup_bytes 复制 value
3. malloc 分配 DbNode
4. 填字段
5. new_node->next = db->buckets[bucket_idx]
6. db->buckets[bucket_idx] = new_node
7. db->size++
```

这里使用的是头插法。

头插法的意思是：新节点直接放到链表最前面。

例如原来：

```text
buckets[5] -> A -> B -> NULL
```

插入新节点 C 后：

```text
buckets[5] -> C -> A -> B -> NULL
```

小结：`db_set` 是唯一真正把新 key/value 长期放进数据库的地方。

### 6.6 GET 的数据库路径：db_get

`db_get`：

```text
1. 根据 key 算桶下标
2. 调用 db_find_live_node 找有效节点
3. 找不到返回 DB_STATUS_NOT_FOUND
4. 找到则把 node->val 和 node->val_len 带给调用方
```

这里返回的是数据库内部 value 指针，不是新复制一份。

为什么可以这样做？

当前命令执行完后会马上调用 `make_resp` 把这个 value 复制进响应缓冲区。在这之间数据库节点还活着，所以可以只读使用这个内部指针。

小结：`db_get` 只读 value，但因为懒删除，它在发现过期 key 时也可能修改数据库。

### 6.7 DEL 的数据库路径：db_del

`db_del`：

```text
1. 算桶下标
2. 调用 db_find_live_node，同时拿到 prev
3. 如果没找到，返回 NOT_FOUND
4. 如果找到，调用 db_unlink_and_free_node
```

`db_unlink_and_free_node` 会处理两种情况：

```text
删除桶头：
db->buckets[bucket_idx] = node->next

删除中间节点：
prev->next = node->next
```

然后释放：

```text
free(node->key)
free(node->val)
free(node)
db->size--
```

小结：`DEL` 的难点在链表摘节点，尤其要区分桶头和中间节点。

### 6.8 EXISTS 的数据库路径：db_exists

`db_exists`：

```text
1. 算桶下标
2. 调用 db_find_live_node
3. 找到返回 OK
4. 找不到返回 NOT_FOUND
```

它不需要 value，所以不会调用 `db_get` 再丢掉 value，而是直接复用底层查找逻辑。

小结：`EXISTS` 是“只查存在性”的轻量查询。

### 6.9 EXPIRE 的数据库路径：db_expire

`db_expire`：

```text
1. 算桶下标
2. 查找有效节点，同时拿到 prev
3. 如果 key 不存在，返回 NOT_FOUND
4. 如果 ttl_ms <= 0，立即删除节点
5. 否则设置：
   node->has_expire = true
   node->expire_at_ms = db_now_ms() + ttl_ms
```

`expire_at_ms` 保存的是绝对过期时刻，不是剩余时间。

例如当前时间是 `100000` 毫秒，执行：

```text
EXPIRE foo 1
```

命令层传给数据库的是 `1000` 毫秒。

数据库保存：

```text
expire_at_ms = 100000 + 1000 = 101000
```

以后只要当前时间 `now_ms >= 101000`，这个 key 就过期。

小结：`EXPIRE` 给节点加上“到某个绝对时间点失效”的信息。

### 6.10 TTL 的数据库路径：db_ttl

`db_ttl`：

```text
1. 算桶下标
2. 查找有效节点
3. 找不到返回 NOT_FOUND
4. 找到但 has_expire == false，返回 NO_EXPIRE
5. 重新取当前时间 now_ms
6. remain = node->expire_at_ms - now_ms
7. remain <= 0 就删除并返回 NOT_FOUND
8. 否则把 remain 写入 out_ttl_ms，返回 OK
```

为什么找到节点后还要再判断一次 `remain <= 0`？

因为时间一直在流动。即使 `db_find_live_node` 刚才判断它没过期，到了后面计算 TTL 时也可能刚好过期。这里再检查一次更稳妥。

小结：`TTL` 把内部绝对过期时刻转换成“还剩多少毫秒”。

## 7. 编解码层运行轨迹

这一节看 `include/codec.h` 和 `src/codec.c`。

### 7.1 请求 payload 格式

当前请求 payload 格式是：

```text
[4字节 argc]
[4字节 arg1_len][arg1_bytes]
[4字节 arg2_len][arg2_bytes]
...
```

例如客户端输入：

```text
SET foo bar
```

拆成 3 个参数：

```text
SET
foo
bar
```

payload 逻辑上是：

```text
argc = 3
arg0_len = 3, arg0 = "SET"
arg1_len = 3, arg1 = "foo"
arg2_len = 3, arg2 = "bar"
```

所有 4 字节整数都用网络字节序保存。

小结：请求 payload 的本质是“参数个数 + 每个参数的长度和字节内容”。

### 7.2 响应类型 RespType

`RespType` 在 `include/codec.h`：

```c
typedef enum {
    RESP_OK = 0,
    RESP_ERR = 1,
    RESP_STR = 2,
    RESP_INT = 3,
    RESP_NIL = 4
} RespType;
```

含义：

- `RESP_OK`：简单成功，例如 `SET` 成功、`SAVE` 成功。
- `RESP_ERR`：错误，例如未知命令、参数数量错误。
- `RESP_STR`：字符串结果，例如 `GET foo` 找到了 value。
- `RESP_INT`：整数结果，例如 `DEL`、`EXISTS`、`TTL`。
- `RESP_NIL`：空结果，例如 `GET missing`。

为什么需要类型？

如果所有响应都只是一段文本，客户端很难区分：

```text
这个 0 是字符串 "0"？
还是整数 0？
这个空结果是错误？
还是 GET 不存在？
```

有了 `RespType` 后，客户端能按类型打印更清楚的结果。

小结：`RespType` 让命令结果不再含糊。

### 7.3 make_resp：服务端把 Resp 编码成字节

服务端执行命令后会调用：

```c
uint8_t *out = make_resp(&resp, &out_len);
```

`make_resp` 返回的是可以直接写到 socket 的完整缓冲区。

注意：它包含最外层 4 字节长度前缀。

响应 body 格式是：

```text
[4字节 type][按 type 决定的后续字段]
```

完整发送格式是：

```text
[4字节 body_len][4字节 type][后续字段]
```

不同响应类型：

```text
RESP_OK:
[body_len=4][type]

RESP_NIL:
[body_len=4][type]

RESP_ERR:
[body_len][type][len][错误文本]

RESP_STR:
[body_len][type][len][字符串内容]

RESP_INT:
[body_len=12][type][8字节整数]
```

例如 `GET foo` 返回 `"bar"`：

```text
body:
[type=RESP_STR][len=3]["bar"]

完整响应:
[body_len=11][type=RESP_STR][len=3]["bar"]
```

`write_i64` 用来把 8 字节整数按大端序写入缓冲区。

小结：`make_resp` 把服务端内部的结果对象变成网络上能传输的字节。

### 7.4 parse_resp：客户端把响应 payload 解析回来

客户端收到响应后调用：

```c
parse_resp(resp_buf, resp_len, &resp)
```

这里的 `resp_buf` 是 `recv_msg` 读到的 payload，不包含最外层长度前缀。

`parse_resp` 的流程：

```text
1. read_u32 读取 type
2. 如果 type 是 OK 或 NIL，后面没有字段
3. 如果 type 是 ERR 或 STR，读取 len，再读取 len 字节 data
4. 如果 type 是 INT，读取 8 字节整数
5. 检查 cur == end，确保没有多余字节
```

解析结果放进 `RespView`：

```c
typedef struct {
    RespType type;
    const uint8_t *data;
    uint32_t len;
    int64_t integer;
} RespView;
```

`RespView` 和 `Resp` 字段很像，但用途不同：

- `Resp`：服务端内部构造“我要返回什么”。
- `RespView`：客户端解析“服务端实际返回了什么”。

小结：`parse_resp` 是客户端理解服务器响应的入口。

## 8. 持久化层运行轨迹

这一节看 `include/persist.h` 和 `src/persist.c`。

当前持久化很克制，只做最小快照：

- 手动执行 `SAVE` 时保存。
- 服务端启动时尝试加载。
- 不做 AOF。
- 不做后台自动保存。
- 不做复杂崩溃恢复。

### 8.1 快照文件格式

当前快照文件名：

```text
my_redis.dump
```

快照文件大致格式：

```text
[4字节 magic: "MYR1"]
[4字节 entry_count]

每条记录：
[4字节 key_len]
[key 字节]
[4字节 val_len]
[value 字节]
[4字节 has_expire]
如果 has_expire == 1:
    [8字节 expire_at_ms]
```

`magic` 是魔数。

为什么要魔数？

加载文件时先检查前 4 字节是不是 `"MYR1"`。如果不是，就说明这个文件很可能不是当前项目的快照格式，应该拒绝加载。

小结：快照文件保存的是 key、value、是否过期、绝对过期时刻。

### 8.2 SAVE 的路径：persist_save

`SAVE` 命令进入 `persist_save(&g_db, SNAPSHOT_FILE)`。

`persist_save` 的流程：

```text
1. fopen(path, "wb") 以二进制写模式打开文件
2. 写入 magic：MYR1
3. 调用 persist_live_entry_count 统计还没过期的节点数量
4. 写入 count
5. 遍历所有桶和桶链节点
6. 如果节点已过期，跳过
7. 写 key_len、key、val_len、value、has_expire
8. 如果有过期时间，再写 expire_at_ms
9. fclose
```

为什么保存前要统计有效节点数量？

因为快照开头要写 `entry_count`。加载时需要知道后面应该读多少条记录。

为什么保存时跳过过期数据？

如果某个 key 在保存那一刻已经过期，就不应该把它写进快照。否则重启后它可能被错误恢复。

小结：`persist_save` 会把当前仍然有效的数据库内容完整写入快照文件。

### 8.3 启动加载路径：persist_load

服务端启动时调用：

```c
persist_load(&g_db, SNAPSHOT_FILE)
```

`persist_load` 的流程：

```text
1. fopen(path, "rb") 打开快照
2. 如果打开失败，返回 1，表示文件不存在或打不开
3. 读取 magic，并检查是否等于 MYR1
4. 读取 count
5. db_init(&tmp) 初始化临时数据库
6. 记录当前时间 now_ms
7. 循环读取 count 条记录：
   - 读 key_len 和 key
   - 读 val_len 和 value
   - 读 has_expire
   - 如果 has_expire，读 expire_at_ms
   - 如果没过期，就 db_set(&tmp, key, value)
   - 如果有过期时间，再 db_expire(&tmp, key, ttl_ms)
   - 释放读文件时临时 malloc 的 key/value
8. 关闭文件
9. db_free(db) 释放旧数据库
10. *db = tmp，把临时数据库替换成主数据库
```

这里有两个很重要的细节。

第一，加载时也会跳过已经过期的数据：

```text
如果 has_expire == 1 并且 expire_at_ms <= now_ms
就不写入 tmp
```

这能避免快照里残留的过期数据被重启“复活”。

第二，加载到临时数据库 `tmp`，成功后才替换主库。

如果文件读到一半失败，函数会：

```text
free 临时 key/value
db_free(&tmp)
fclose(fp)
return -1
```

主数据库 `g_db` 不会被半途污染。

小结：`persist_load` 是一个“先安全恢复到临时库，再替换主库”的启动恢复过程。

## 9. 客户端运行轨迹

现在看 `src/tcp_client.c`。

客户端比服务端简单很多，因为它只维护一个连接，而且用阻塞式 `send_msg` / `recv_msg`。

### 9.1 客户端 main：连接服务端

客户端入口：

```c
int main(void)
```

先创建 socket：

```c
int fd = socket(AF_INET, SOCK_STREAM, 0);
```

这里和服务端一样：

- `fd` 是 socket 文件描述符。
- `AF_INET` 表示 IPv4。
- `SOCK_STREAM` 表示 TCP。

然后准备地址：

```c
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
addr.sin_family = AF_INET;
addr.sin_port = htons(15001);
addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
```

这里和服务端不同的是：

- 服务端用 `INADDR_ANY`，表示监听所有网卡。
- 客户端用 `INADDR_LOOPBACK`，表示连接本机回环地址，也就是 `127.0.0.1`。

然后连接：

```c
connect(fd, (struct sockaddr *)&addr, sizeof(addr))
```

小结：客户端启动后会连接到本机 `15001` 端口上的服务端。

### 9.2 读取终端输入

客户端进入循环：

```c
char line[4096];
while (1) {
    printf("> ");
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        break;
    ...
}
```

`line` 是一块 4096 字节的栈缓冲区，用来保存你输入的一行命令。

`fgets` 会读取一行，包括末尾换行符。

后面会去掉换行：

```c
size_t n = strlen(line);
if (n && line[n - 1] == '\n') {
    line[n - 1] = 0;
    n--;
}
```

小结：客户端把你在终端输入的一行文本读进 `line`。

### 9.3 build_req：把文本命令变成请求 payload

客户端调用：

```c
build_req(line, &req, &req_len)
```

`build_req` 的输入：

- `line`：可修改的一行文本，例如 `"SET foo bar"`。

`build_req` 的输出：

- `req`：新分配的请求 payload 缓冲区。
- `req_len`：payload 长度。

它先准备：

```c
char *argv[16];
uint32_t lens[16];
uint32_t argc = 0;
```

名字解释：

- `argv`：参数数组，每个元素指向一个 token。
- `lens`：每个参数的长度。
- `argc`：参数个数。

然后用：

```c
strtok(line, " ")
```

按空格切分。

例如：

```text
输入行：SET foo bar

切分后：
argv[0] = "SET", lens[0] = 3
argv[1] = "foo", lens[1] = 3
argv[2] = "bar", lens[2] = 3
argc = 3
```

注意当前客户端的限制：

```text
它只按空格切分，不支持 value 里带空格。
```

例如：

```text
SET msg hello world
```

会被切成 4 个参数，而不是把 `"hello world"` 当成一个 value。

这是当前阶段刻意保留的简化点。

接着计算 payload 总长度：

```c
uint32_t total = 4;
for (uint32_t i = 0; i < argc; i++) {
    total += 4;
    total += lens[i];
}
```

为什么一开始是 4？

因为 payload 开头要放 `argc`，它是 4 字节。

每个参数又需要：

```text
4 字节参数长度 + 参数内容字节
```

然后分配缓冲区并写入：

```text
写 argc
写 arg0_len 和 arg0 字节
写 arg1_len 和 arg1 字节
...
```

所有 4 字节整数都通过 `htonl` 转成网络字节序。

小结：`build_req` 把人输入的文本命令变成服务端能解析的二进制 payload。

### 9.4 send_msg：发送完整消息

客户端调用：

```c
send_msg(fd, req, req_len)
```

这个函数在 `src/proto.c`。

`send_msg` 做两件事：

```text
1. 先发送 4 字节 payload 长度
2. 再发送 payload 本身
```

它内部用 `write_all`。

`write_all` 的作用是：不断 `write`，直到缓冲区里的 `n` 个字节全部写出去。

为什么不能假设一次 `write` 就写完？

和 `read` 一样，socket 写入也可能一次只写了一部分。`write_all` 用循环保证完整发送。

小结：客户端通过 `send_msg` 把一条完整请求发到服务端。

### 9.5 recv_msg：接收完整响应

客户端发完请求后调用：

```c
recv_msg(fd, &resp_buf, &resp_len)
```

`recv_msg` 也在 `src/proto.c`。

流程：

```text
1. read_full 读 4 字节响应 body 长度
2. ntohl 转成本机字节序
3. 检查不能超过 MAX_MSG_SIZE
4. malloc(len + 1)
5. read_full 读完整 payload
6. 末尾补 0，方便调试
7. 把指针和长度返回给调用方
```

`read_full` 的作用是：不断 `read`，直到读满指定字节数。

小结：客户端通过 `recv_msg` 得到一整条响应 payload。

### 9.6 parse_resp 和 print_resp

收到响应 payload 后：

```c
RespView resp;
if (parse_resp(resp_buf, resp_len, &resp) != 0) {
    printf("(bad response)\n");
    free(resp_buf);
    continue;
}

print_resp(&resp);
```

`parse_resp` 在 `src/codec.c`，前面已经讲过。

`print_resp` 在 `src/tcp_client.c`：

```c
case RESP_OK:
    printf("< OK\n");
    break;

case RESP_ERR:
    printf("< (ERR) ...\n");
    break;

case RESP_STR:
    printf("< %.*s\n", ...);
    break;

case RESP_INT:
    printf("< %lld\n", ...);
    break;

case RESP_NIL:
    printf("< (nil)\n");
    break;
```

所以不同响应会显示成：

```text
RESP_OK   -> < OK
RESP_ERR  -> < (ERR) wrong number of arguments
RESP_STR  -> < bar
RESP_INT  -> < 1
RESP_NIL  -> < (nil)
```

小结：客户端最后把协议里的类型结果翻译成人能看懂的输出。

## 10. 一条完整命令的示例跟踪

这一节把前面的模块串起来看。

### 10.1 示例一：SET foo bar

你在客户端输入：

```text
SET foo bar
```

#### 客户端阶段

第一站：`src/tcp_client.c`

`main` 读到这行文本后去掉换行，然后调用：

```c
build_req(line, &req, &req_len)
```

`build_req` 用空格切分：

```text
argc = 3
argv[0] = "SET", lens[0] = 3
argv[1] = "foo", lens[1] = 3
argv[2] = "bar", lens[2] = 3
```

编码 payload：

```text
[argc=3]
[len=3]["SET"]
[len=3]["foo"]
[len=3]["bar"]
```

然后 `send_msg` 在最前面再加 4 字节 payload 长度，通过 TCP 发给服务端。

#### 服务端读取阶段

第二站：`src/tcp_server.c`

`poll` 发现客户端连接可读，进入：

```c
conn_on_readable(c)
```

它先读 4 字节长度到 `c->lenbuf`，解析出 `c->body_len`。

然后分配 `c->body`，继续读完整 payload。

#### 服务端解析阶段

第三站：`src/codec.c`

服务端调用：

```c
parse_req(c->body, c->body_len, &args)
```

解析后：

```text
args.argc = 3
args.argv[0] -> "SET"
args.argv[1] -> "foo"
args.argv[2] -> "bar"
```

注意这些 `argv` 指针都指向 `c->body` 内部。

#### 命令执行阶段

第四站：`src/tcp_server.c`

服务端进入：

```c
do_request(&args, &resp)
```

它识别出 `argv[0]` 是 `SET`，检查参数数量为 3，然后调用：

```c
db_set(&g_db, "foo", 3, "bar", 3)
```

第五站：`src/db.c`

`db_set`：

```text
1. 检查是否需要 rehash
2. 计算 "foo" 的桶下标
3. 在这个桶的链表里查找是否已有 "foo"
4. 如果没有，就复制 key 和 value
5. 分配一个 DbNode
6. 填入 key="foo", val="bar"
7. has_expire=false, expire_at_ms=0
8. 头插到对应桶链
9. db->size++
```

执行后，数据库里多了一条记录：

```text
key = "foo"
value = "bar"
没有过期时间
```

`db_set` 返回 `DB_STATUS_OK`。

第六站：回到 `src/tcp_server.c`

`do_request` 设置：

```text
resp.type = RESP_OK
```

#### 响应阶段

第七站：`src/codec.c`

服务端调用：

```c
make_resp(&resp, &out_len)
```

`RESP_OK` 没有额外数据，所以响应 body 只有 4 字节 type。

完整响应大致是：

```text
[body_len=4][type=RESP_OK]
```

第八站：`src/tcp_server.c`

`conn_on_readable` 把响应放到：

```text
c->wbuf
c->wlen
c->wsent = 0
c->st = ST_WRITE_RESP
```

之后 `poll` 发现连接可写，进入 `conn_on_writable`，把响应写回客户端。写完后回到 `ST_READ_LEN`，等待下一条请求。

#### 客户端打印阶段

第九站：`src/tcp_client.c`

客户端 `recv_msg` 收到响应 payload。

然后 `parse_resp` 解析出：

```text
type = RESP_OK
```

`print_resp` 打印：

```text
< OK
```

小结：`SET foo bar` 最终把 `"foo" -> "bar"` 存进数据库，并返回简单成功。

### 10.2 示例二：GET foo

你输入：

```text
GET foo
```

客户端编码：

```text
argc = 2
argv[0] = "GET"
argv[1] = "foo"
```

服务端读完整请求后，`parse_req` 得到同样的 `Args`。

`do_request` 识别 `GET`，调用：

```c
db_get(&g_db, "foo", 3, &out->data, &out->len)
```

`db_get`：

```text
1. 根据 "foo" 算桶下标
2. 调用 db_find_live_node
3. 找到 DbNode("foo" -> "bar")
4. 检查它没有过期
5. 把 node->val 指针和 node->val_len 返回
```

回到 `do_request` 后：

```text
out->type = RESP_STR
out->data -> "bar"
out->len = 3
```

`make_resp` 编码：

```text
[body_len=11][type=RESP_STR][len=3]["bar"]
```

客户端解析后：

```text
type = RESP_STR
data = "bar"
len = 3
```

打印：

```text
< bar
```

小结：`GET foo` 通过哈希表查到 value，响应类型是字符串。

### 10.3 示例三：GET missing

你输入：

```text
GET missing
```

客户端编码：

```text
argc = 2
argv[0] = "GET"
argv[1] = "missing"
```

服务端 `do_request` 调用：

```c
db_get(&g_db, "missing", 7, &out->data, &out->len)
```

`db_get`：

```text
1. 算 "missing" 的桶下标
2. 扫描对应桶链
3. 没找到 key
4. 返回 DB_STATUS_NOT_FOUND
```

回到 `do_request`：

```text
out->type = RESP_NIL
```

为什么不是错误？

因为 `GET missing` 这个命令本身执行成功了，只是查询结果为空。

`make_resp` 编码：

```text
[body_len=4][type=RESP_NIL]
```

客户端打印：

```text
< (nil)
```

小结：不存在的 key 用 `RESP_NIL` 表示，而不是 `RESP_ERR`。

### 10.4 示例四：EXISTS foo

你输入：

```text
EXISTS foo
```

客户端编码：

```text
argc = 2
argv[0] = "EXISTS"
argv[1] = "foo"
```

服务端 `do_request` 调用：

```c
db_exists(&g_db, "foo", 3)
```

如果前面已经执行过 `SET foo bar`，并且它还没过期：

```text
db_exists -> db_find_live_node -> 找到有效节点 -> DB_STATUS_OK
```

`do_request` 设置：

```text
out->type = RESP_INT
out->integer = 1
```

`make_resp` 编码：

```text
[body_len=12][type=RESP_INT][integer=1]
```

客户端打印：

```text
< 1
```

如果 `foo` 不存在，`db_exists` 返回 `DB_STATUS_NOT_FOUND`，客户端会看到：

```text
< 0
```

小结：`EXISTS` 不返回 value，只返回整数 1 或 0。

### 10.5 示例五：EXPIRE foo 1，然后过期后再 GET foo

先确保有 key：

```text
SET foo bar
```

然后输入：

```text
EXPIRE foo 1
```

客户端编码：

```text
argc = 3
argv[0] = "EXPIRE"
argv[1] = "foo"
argv[2] = "1"
```

服务端 `do_request` 先调用：

```c
arg_to_i64("1", 1, &seconds)
```

得到：

```text
seconds = 1
```

然后换算成毫秒：

```text
ttl_ms = 1 * 1000 = 1000
```

再调用：

```c
db_expire(&g_db, "foo", 3, 1000)
```

`db_expire`：

```text
1. 算 "foo" 的桶下标
2. 找到有效节点 DbNode("foo" -> "bar")
3. ttl_ms > 0，所以不删除
4. 设置 has_expire = true
5. 设置 expire_at_ms = 当前毫秒时间 + 1000
```

`do_request` 返回：

```text
RESP_INT, integer = 1
```

客户端打印：

```text
< 1
```

等待超过 1 秒后，再输入：

```text
GET foo
```

服务端调用：

```c
db_get(&g_db, "foo", 3, &out->data, &out->len)
```

这次 `db_find_live_node` 会：

```text
1. 找到 "foo" 节点
2. 发现 node->has_expire == true
3. 发现 node->expire_at_ms <= 当前时间
4. 调用 db_unlink_and_free_node 把节点从桶链表删除
5. 返回 NULL
```

`db_get` 返回 `DB_STATUS_NOT_FOUND`。

`do_request` 设置：

```text
RESP_NIL
```

客户端打印：

```text
< (nil)
```

小结：过期 key 不会主动消失，而是在访问时被懒删除；过期后的 `GET` 就像 key 不存在。

### 10.6 示例六：SAVE 后重启再 GET foo

这里要注意一个顺序问题。

如果你刚刚执行过：

```text
EXPIRE foo 1
```

并且已经等到 `foo` 过期，那么 `foo` 已经不存在了。此时再 `SAVE`，快照里不会保存 `foo`。

所以为了演示 “SAVE 后重启再 GET foo”，我们先重新写入一个有效 key：

```text
SET foo bar
SAVE
```

#### SAVE 阶段

`SAVE` 的客户端编码：

```text
argc = 1
argv[0] = "SAVE"
```

服务端 `do_request` 调用：

```c
persist_save(&g_db, "my_redis.dump")
```

`persist_save`：

```text
1. 打开 my_redis.dump
2. 写入 magic：MYR1
3. 统计当前仍然有效的节点数量
4. 写入 count
5. 遍历所有桶
6. 找到 "foo" -> "bar"
7. 如果它没有过期，就写入：
   key_len = 3
   key = "foo"
   val_len = 3
   val = "bar"
   has_expire = 0
8. 关闭文件
```

保存成功后服务端返回：

```text
RESP_OK
```

客户端打印：

```text
< OK
```

#### 重启服务端阶段

你停止服务端后重新运行：

```bash
./server
```

服务端启动时仍然从 `src/tcp_server.c` 的 `main` 开始。

它先：

```text
socket
setsockopt
bind
listen
db_init(&g_db)
```

这时 `g_db` 先变成空库。

然后执行：

```c
persist_load(&g_db, SNAPSHOT_FILE)
```

`persist_load` 打开 `my_redis.dump`：

```text
1. 读 magic，确认是 MYR1
2. 读 count
3. 初始化临时数据库 tmp
4. 读出 key="foo", value="bar", has_expire=0
5. 调用 db_set(&tmp, "foo", 3, "bar", 3)
6. 全部读完后，db_free(&g_db)
7. *db = tmp
```

此时重启后的 `g_db` 又有了：

```text
"foo" -> "bar"
```

#### 重启后 GET 阶段

客户端重新连接，输入：

```text
GET foo
```

执行路径和前面的 `GET foo` 一样：

```text
tcp_client.c build_req
proto.c send_msg
tcp_server.c conn_on_readable
codec.c parse_req
tcp_server.c do_request
db.c db_get
codec.c make_resp
tcp_server.c conn_on_writable
proto.c recv_msg
codec.c parse_resp
tcp_client.c print_resp
```

因为 `persist_load` 已经恢复了 `foo`，所以客户端打印：

```text
< bar
```

小结：`SAVE` 把有效 key 写入快照；重启时 `persist_load` 把它恢复进内存数据库；之后 `GET` 就能重新查到。

## 11. 旧逻辑和当前主线的边界

`src/tcp_server.c` 里还有一个函数：

```c
static int conn_make_resp(Conn *c, const uint8_t *req, uint32_t req_len)
```

这是旧阶段遗留的 PING/ECHO 风格响应逻辑。

当前主线不走它。

当前主线是：

```text
conn_on_readable
  -> parse_req
  -> do_request
  -> make_resp
  -> conn_on_writable
```

所以你第一次复习源码时，可以先跳过 `conn_make_resp`。等理解主线后，再回头把它当作旧版本对照材料看。

`archive/` 目录也是类似定位：它保存旧章节实现和旧计划文档，不是当前可运行主线。

小结：先抓当前主线，不要被旧演进痕迹带偏。

## 12. 项目主线总结

当前项目最核心的数据流可以压缩成一句话：

```text
客户端把一行命令编码成二进制请求，服务端用非阻塞状态机读完整请求，解析成 Args，分发到 db/persist，得到 Resp，再编码成二进制响应写回客户端。
```

最关键的结构体：

```text
Conn
服务端每条客户端连接的读写状态。

Args
请求解析后的参数视图，指向请求 body 内部。

Resp
服务端命令执行后的响应语义对象。

RespView
客户端解析响应后的结果视图。

Db
整个内存数据库，里面有桶数组、当前桶数、key 数量。

DbNode
一条 key-value 节点，带 key/value、长度、过期时间、next 指针。
```

最关键的函数：

```text
src/tcp_server.c
main               服务端启动和 poll 事件循环
conn_on_readable   读取完整请求、解析、执行、准备响应
do_request         命令分发：SET / GET / DEL / EXISTS / EXPIRE / TTL / SAVE
conn_on_writable   把响应写回客户端

src/tcp_client.c
main               客户端连接服务端、循环读输入和打印响应
build_req          把文本命令编码成请求 payload
print_resp         按响应类型打印结果

src/codec.c
parse_req          请求 payload -> Args
make_resp          Resp -> 可发送的响应字节
parse_resp         响应 payload -> RespView

src/db.c
db_set             写入或覆盖 key-value
db_get             查询 value
db_del             删除 key
db_exists          判断 key 是否存在
db_expire          设置过期时间
db_ttl             查询剩余过期时间
db_find_live_node  查找有效节点，顺带处理过期懒删除

src/persist.c
persist_save       保存快照
persist_load       启动时加载快照

src/proto.c
send_msg           客户端发送 [长度][payload]
recv_msg           客户端接收 [长度][payload]

src/nb.c
set_nonblocking    把 socket 设置成非阻塞
```

初学者最容易卡住的点：

```text
1. TCP 是字节流，不保证一次 read 就是一条完整命令。
2. 服务端用 Conn 保存读写进度，是为了处理非阻塞 I/O。
3. Args 里的 argv 不是复制出来的字符串，只是指向请求 body 的视图。
4. 数据库必须复制 key/value，因为请求 body 很快会被释放。
5. 哈希表不是只算 hash 就完事，桶链里还要精确比较 key。
6. rehash 是重新分配节点所在桶，不是重新创建所有 key/value。
7. 过期时间保存的是绝对时刻，不是剩余秒数。
8. 过期 key 当前是懒删除，访问时才会被清理。
9. RESP_NIL 和 RESP_ERR 不一样，GET 不存在不是命令错误。
10. 持久化加载先写入临时 Db，成功后才替换主 Db。
```

建议你接下来按这个顺序复习源码：

```text
1. src/tcp_server.c 的 main
2. src/tcp_server.c 的 Conn 和 ConnState
3. src/tcp_server.c 的 conn_on_readable
4. include/codec.h 的 Args / Resp / RespView / RespType
5. src/codec.c 的 parse_req 和 make_resp
6. src/tcp_server.c 的 do_request
7. include/db.h 的 Db 和 DbNode
8. src/db.c 的 db_set、db_find_live_node、db_get、db_expire、db_ttl
9. src/persist.c 的 persist_save 和 persist_load
10. src/tcp_client.c 的 build_req、main、print_resp
11. src/proto.c 的 send_msg 和 recv_msg
12. src/nb.c 的 set_nonblocking
```

如果你只想先走一遍最重要的主线，就按下面这条线看：

```text
src/tcp_server.c: main
  -> conn_on_readable
  -> src/codec.c: parse_req
  -> src/tcp_server.c: do_request
  -> src/db.c: db_set / db_get / db_expire / db_ttl
  -> src/codec.c: make_resp
  -> src/tcp_server.c: conn_on_writable
```

这一条线走通后，再回来看客户端和持久化，会轻松很多。
