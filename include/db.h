#pragma once

/*
 * 本文件属于哪一章：
 * - ch07：围绕字符串 KV 主线，加入最小过期时间能力
 *
 * 本文件负责什么：
 * - 定义当前章节“内存数据库”的对外接口
 * - 约定哈希桶节点长什么样
 * - 约定数据库整体结构里需要记录哪些运行时信息
 * - 约定服务端调用数据库时能收到哪些状态码
 *
 * 它和其他文件如何配合：
 * - src/db.c：真正实现这里声明的可扩容哈希表、删除逻辑、存在性检查和过期时间
 * - src/tcp_server.c：在命令执行阶段调用 db_set / db_get / db_del / db_exists / db_expire / db_ttl
 * - src/codec.c：继续负责请求解析和响应打包，不关心底层存储细节
 *
 * 这一章和上一章最大的区别：
 * - 上一章重点是“怎样只判断 key 存不存在”
 * - 这一章重点是“怎样让 key 在未来某个时间点后自动失效”
 * - 数据库节点会第一次真正带上时间相关字段
 *
 * 为什么仍然尽量保持同一套接口名字？
 * - 因为这样能突出“接口稳定，实现升级”的工程思路
 * - tcp_server.c 不需要知道底层已经从固定桶数升级成可扩容版本
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define DB_MAX_KV 1024 /* 当前章节最多保存 1024 个键值对 */
#define DB_INITIAL_BUCKET_COUNT 256 /* 数据库初始化时先使用 256 个桶 */
#define DB_MAX_BUCKET_COUNT 2048 /* 当前章节允许扩到 2048 个桶 */

/*
 * DbNode 表示哈希表桶链中的一个节点。
 *
 * 字段说明：
 * - key / key_len：键的字节内容和长度
 * - val / val_len：值的字节内容和长度
 * - has_expire：这个 key 当前有没有设置过期时间
 * - expire_at_ms：如果设置了过期时间，这里保存“绝对过期时刻（毫秒）”
 * - next：同一个桶中的下一个节点
 *
 * 为什么这里同时保存指针和长度，而不是只保存 char *？
 * - 因为只靠 '\0' 结尾来判断字符串边界并不稳妥
 * - 显式保存长度后，比较 key 时可以做到“长度相等且内容相等”
 * - 这样不会把 "foo" 和 "foobar" 这种前缀相同的 key 误判成相同
 *
 * 对初学者最重要的理解点：
 * - 数据库存的不是“外面传进来的临时指针”
 * - 而是“数据库自己复制并长期持有”的数据副本
 *
 * 为什么会多出 next 指针？
 * - 因为多个 key 可能被哈希到同一个桶
 * - 当冲突发生时，我们先用最容易讲清楚的方式：链表挂接
 */
typedef struct DbNode {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
    bool has_expire;
    int64_t expire_at_ms;
    struct DbNode *next;
} DbNode;

/*
 * Db 表示当前章节的整个内存数据库。
 *
 * 字段说明：
 * - buckets：哈希桶数组。每个桶里放一条链表的头指针
 * - bucket_count：当前真正启用的桶数量
 * - size：当前数据库里一共有多少个键值对
 *
 * 为什么这一章要在结构体里增加 bucket_count？
 * - 因为桶数量不再固定
 * - 初始化时只启用一部分桶
 * - 当数据变多后，会扩到更多桶并触发 rehash
 *
 * 这里的 buckets 数组本身并不直接保存数据，
 * 它保存的是“每个桶链表的入口”。
 *
 * 为什么 buckets 仍然是一个固定上限数组，而不是动态分配？
 * - 这是本章为了降低学习复杂度而做的教学取舍
 * - 我们先把“扩容 / rehash 的思路”讲清楚
 * - 先不把问题扩展到“桶数组本身的堆内存管理”
 */
typedef struct {
    DbNode *buckets[DB_MAX_BUCKET_COUNT];
    size_t bucket_count;
    size_t size;
} Db;

/*
 * DbStatus 表示数据库操作结果。
 *
 * 为什么不直接只用 0 / -1？
 * - 因为“失败”有很多种原因
 * - 如果把所有失败都混成一个值，命令层就看不出到底是“没找到”还是“存满了”
 *
 * 当前几个状态值的含义：
 * - DB_STATUS_OK：操作成功
 * - DB_STATUS_NOT_FOUND：读取时没找到 key
 * - DB_STATUS_FULL：新增记录时数据库容量已满
 * - DB_STATUS_OOM：向堆申请内存失败
 * - DB_STATUS_NO_EXPIRE：key 存在，但当前没有设置过期时间
 */
typedef enum {
    DB_STATUS_OK = 0,
    DB_STATUS_NOT_FOUND = 1,
    DB_STATUS_FULL = 2,
    DB_STATUS_OOM = 3,
    DB_STATUS_NO_EXPIRE = 4
} DbStatus;

/*
 * 初始化数据库，让它进入“空库”状态。
 *
 * 参数：
 * - db：要初始化的数据库对象
 *
 * 返回值：
 * - 无
 *
 * 数据流：
 * - 调用后，前 DB_INITIAL_BUCKET_COUNT 个桶视为当前激活桶
 * - 所有桶都为空
 * - size 会变成 0
 */
void db_init(Db *db);

/*
 * 释放数据库里所有动态分配的内存。
 *
 * 参数：
 * - db：要释放的数据库对象
 *
 * 返回值：
 * - 无
 *
 * 为什么即使当前服务端几乎不会退出，也仍然要提供这个接口？
 * - 从模块设计上，init/free 成对更完整
 * - 后面做独立小测试时，会非常方便
 * - 可以帮助你养成“谁申请资源，谁负责释放”的好习惯
 */
void db_free(Db *db);

/*
 * 写入一个 key/value。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要写入的键
 * - val / val_len：要写入的值
 *
 * 行为约定：
 * - 如果 key 不存在，就新增一条记录
 * - 如果 key 已存在，就覆盖旧值
 *
 * 返回值：
 * - DB_STATUS_OK：写入成功
 * - DB_STATUS_FULL：数据库容量已满，无法新增
 * - DB_STATUS_OOM：内存分配失败
 *
 * 易错点：
 * - key 和 val 来自请求缓冲区，它们本身不会长期有效
 * - 所以实现里必须复制出数据库自己的副本，而不能直接保存外部指针
 * - 如果 key 哈希冲突到同一个桶，不能把旧节点覆盖掉，而要继续沿链表查找
 * - 如果当前负载已经过高，可能会在写入前先触发 rehash
 */
DbStatus db_set(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t *val,
                uint32_t val_len);

/*
 * 读取一个 key 对应的 value。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要查找的键
 * - out_val / out_len：用于带出查找到的值及其长度
 *
 * 返回值：
 * - DB_STATUS_OK：找到并返回 value
 * - DB_STATUS_NOT_FOUND：不存在这个 key
 *
 * 输出说明：
 * - out_val / out_len 会直接指向数据库节点内部保存的 value
 * - 调用方只读使用即可，不应该在外部修改这块内存
 *
 * 易错点：
 * - 这里返回的是“内部指针”，不是新复制的一份数据
 * - 如果未来数据库支持删除、rehash 或扩容迁移，就要特别小心这类指针的生命周期
 */
DbStatus db_get(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t **out_val,
                uint32_t *out_len);

/*
 * 删除一个 key 及其对应的 value。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要删除的键
 *
 * 返回值：
 * - DB_STATUS_OK：成功找到并删除
 * - DB_STATUS_NOT_FOUND：数据库中不存在这个 key
 *
 * 数据流：
 * - 先根据 key 算出它当前所在的桶
 * - 再在该桶链表中顺着 next 逐个查找
 * - 找到后把节点从链表中摘掉
 * - 最后释放节点内部持有的 key/value 和节点本身
 *
 * 易错点：
 * - 删除链表节点时，要同时处理“删除桶头节点”和“删除中间节点”两种情况
 * - 删除成功后，数据库总元素个数 size 也要同步减一
 * - 释放顺序不能漏，否则会造成内存泄漏
 */
DbStatus db_del(Db *db, const uint8_t *key, uint32_t key_len);

/*
 * 判断某个 key 是否存在。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要查询的键
 *
 * 返回值：
 * - DB_STATUS_OK：key 存在
 * - DB_STATUS_NOT_FOUND：key 不存在
 *
 * 为什么这里仍然返回 DbStatus，而不是单独定义一个 bool 接口？
 * - 因为当前项目命令层本来就已经在使用 DbStatus
 * - 这样可以继续保持接口风格统一
 * - 对初学者来说，也更容易把“查询存在性”和“查询取值”放在同一套结果体系里理解
 *
 * 数据流：
 * - 先算出 key 所属桶
 * - 再只扫描这个桶链
 * - 找到就返回 OK，没找到就返回 NOT_FOUND
 */
DbStatus db_exists(Db *db, const uint8_t *key, uint32_t key_len);

/*
 * 给一个已存在的 key 设置过期时间。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：目标键
 * - ttl_ms：距离现在还有多少毫秒后过期
 *
 * 返回值：
 * - DB_STATUS_OK：成功设置过期时间
 * - DB_STATUS_NOT_FOUND：key 不存在
 *
 * 调用时机：
 * - 命令层收到 EXPIRE 之后
 *
 * 注意：
 * - 这里保存的是“绝对过期时刻”，不是“剩余秒数”
 * - 这样后面查询 TTL 或做过期判断时会更直接
 */
DbStatus db_expire(Db *db,
                   const uint8_t *key,
                   uint32_t key_len,
                   int64_t ttl_ms);

/*
 * 查询一个 key 还剩多少毫秒过期。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：目标键
 * - out_ttl_ms：返回剩余毫秒数
 *
 * 返回值：
 * - DB_STATUS_OK：key 存在，且设置了过期时间
 * - DB_STATUS_NO_EXPIRE：key 存在，但没有设置过期时间
 * - DB_STATUS_NOT_FOUND：key 不存在，或者已经过期并被视为不存在
 *
 * 为什么这里返回毫秒，而不是秒？
 * - 因为数据库内部通常更适合保存更细粒度的时间
 * - 命令层如果只想按秒显示，可以自己再做换算
 */
DbStatus db_ttl(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                int64_t *out_ttl_ms);
