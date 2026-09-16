/*
 * 本文件属于哪一章：
 * - ch07：围绕字符串 KV 主线，加入最小过期时间能力
 *
 * 本文件负责什么：
 * - 实现当前章节的内存数据库
 * - 继续提供 db_init / db_set / db_get / db_del / db_exists / db_free
 * - 新增 db_expire / db_ttl，让 key 可以带过期时间
 *
 * 它和其他文件如何配合：
 * - include/db.h：声明本文件实现的接口和结构
 * - src/tcp_server.c：收到命令后调用本文件完成真正的数据读写、删除、存在性判断和过期时间处理
 * - 其他网络和协议文件继续不关心底层存储细节
 *
 * 这一章最重要的学习点：
 * - key 并不一定永远存在
 * - 一旦引入过期时间，读 / 查 / 删 都必须考虑“这个 key 可能已经超时了”
 * - 当前章节先采用最容易讲清楚的“懒删除式过期检查”
 *
 * 这一章不会去实现复杂的后台过期线程。
 * 我们只先实现“最小过期时间能力”：
 * - 可以给 key 设置过期时间
 * - 再访问时如果已经过期，就把它当成不存在
 */

#include "db.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 前置声明，方便后面的辅助函数提前复用节点释放逻辑。 */
static void db_free_node(DbNode *node);

/*
 * 判断一个哈希节点里的 key，是否和外部传入的 key 表示“同一个键”。
 *
 * 参数：
 * - node：哈希桶链表中的一个节点
 * - key / key_len：外部正在查找或写入的键
 *
 * 返回值：
 * - 1：是同一个 key
 * - 0：不是同一个 key
 *
 * 为什么必须同时检查长度和内容？
 * - 如果只比较前 key_len 个字节，"foo" 和 "foobar" 容易被前缀误判
 * - 只有“长度相等且内容相等”才表示同一个 key
 */
static int db_key_equals(const DbNode *node,
                         const uint8_t *key,
                         uint32_t key_len)
{
    if (node->key_len != key_len)
        return 0;

    if (key_len == 0)
        return 1;

    return memcmp(node->key, key, key_len) == 0;
}

/*
 * 计算一段 key 的哈希值。
 *
 * 参数：
 * - key / key_len：目标 key
 *
 * 返回值：
 * - 返回一个 32 位无符号整数哈希值
 *
 * 为什么本章单独写一个哈希函数？
 * - 因为哈希表的第一步就是“把任意 key 映射成数字”
 * - 后面我们会用这个数字决定它落到哪个桶里
 *
 * 这里选用的是一个非常常见、容易讲清楚的 FNV-1a 风格写法。
 * 你现在不需要死记常量，只需要理解：
 * - 每个字节都会参与运算
 * - 最终得到一个比较分散的整数
 */
static uint32_t db_hash_key(const uint8_t *key, uint32_t key_len)
{
    uint32_t hash = 2166136261u;

    for (uint32_t i = 0; i < key_len; i++) {
        hash ^= key[i];
        hash *= 16777619u;
    }

    return hash;
}

/*
 * 根据 key 计算它应该落到哪个桶。
 *
 * 参数：
 * - key / key_len：目标 key
 * - bucket_count：这次计算应当基于多少个活跃桶
 *
 * 返回值：
 * - 一个 0 到 bucket_count - 1 之间的桶下标
 *
 * 为什么这里要取模？
 * - 因为哈希值范围很大
 * - 而当前真正启用的桶数量是有限的
 * - 取模后才能把它映射到“当前活跃桶范围”内
 */
static size_t db_bucket_index_with_count(const uint8_t *key,
                                         uint32_t key_len,
                                         size_t bucket_count)
{
    return (size_t)(db_hash_key(key, key_len) % bucket_count);
}

/*
 * 在某个桶的链表中查找指定 key 对应的节点。
 *
 * 参数：
 * - db：目标数据库
 * - bucket_idx：已经算好的桶下标
 * - key / key_len：目标 key
 *
 * 返回值：
 * - 找到：返回对应节点指针
 * - 没找到：返回 NULL
 *
 * 数据流：
 * - 先拿到这个桶的链表头
 * - 再沿着 next 指针逐个向后找
 *
 * 易错点：
 * - “哈希表查找更快”不代表完全不比较 key
 * - 哈希值相同只是说明“落到同一个桶”，最后仍然要精确比较 key 内容
 */
/*
 * 取得当前时间的毫秒值。
 *
 * 返回值：
 * - 从 Unix epoch 开始经过的毫秒数
 *
 * 为什么这里选择绝对时间，而不是“还剩多少秒”？
 * - 因为数据库内部更适合保存“到哪个时间点过期”
 * - 这样后面判断是否过期时，只需要比较 now 和 expire_at_ms
 */
static int64_t db_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * 判断一个节点是否已经过期。
 *
 * 参数：
 * - node：目标节点
 * - now_ms：当前时间，单位毫秒
 *
 * 返回值：
 * - 1：已经过期
 * - 0：还没过期，或者根本没设置过期时间
 */
static int db_node_is_expired(const DbNode *node, int64_t now_ms)
{
    return node->has_expire && node->expire_at_ms <= now_ms;
}

/*
 * 把某个节点从桶链中摘掉，并释放它占用的资源。
 *
 * 参数：
 * - db：目标数据库
 * - bucket_idx：该节点所在桶下标
 * - prev：前一个节点；如果删的是桶头，这里传 NULL
 * - node：要删除的目标节点
 *
 * 返回值：
 * - 无
 *
 * 调用时机：
 * - db_del 真正删除 key 时
 * - 发现目标 key 已经过期，需要懒删除时
 *
 * 易错点：
 * - 删桶头时要改桶头指针
 * - 删中间节点时要改 prev->next
 * - 删除成功后 size 也必须同步减一
 */
static void db_unlink_and_free_node(Db *db,
                                    size_t bucket_idx,
                                    DbNode *prev,
                                    DbNode *node)
{
    if (prev == NULL) {
        db->buckets[bucket_idx] = node->next;
    } else {
        prev->next = node->next;
    }

    db_free_node(node);
    db->size--;
}

/*
 * 在目标桶中查找“仍然有效”的节点。
 *
 * 参数：
 * - db：目标数据库
 * - bucket_idx：已经算好的桶下标
 * - key / key_len：目标键
 * - out_prev：如果找到节点，返回它前一个节点；找不到则返回 NULL
 *
 * 返回值：
 * - 找到且还没过期：返回节点指针
 * - 没找到：返回 NULL
 * - 找到但已经过期：会先执行懒删除，然后返回 NULL
 *
 * 为什么这一章要增加这个辅助函数？
 * - 因为从 ch07 开始，“找到 key”并不代表它还能用
 * - 还要再判断它是不是已经过期
 * - 这个逻辑会在 GET / EXISTS / DEL / EXPIRE / TTL 里反复出现
 */
static DbNode *db_find_live_node(Db *db,
                                 size_t bucket_idx,
                                 const uint8_t *key,
                                 uint32_t key_len,
                                 DbNode **out_prev)
{
    int64_t now_ms = db_now_ms();
    DbNode *prev = NULL;
    DbNode *node = db->buckets[bucket_idx];

    while (node) {
        if (db_key_equals(node, key, key_len)) {
            if (db_node_is_expired(node, now_ms)) {
                db_unlink_and_free_node(db, bucket_idx, prev, node);
                if (out_prev)
                    *out_prev = NULL;
                return NULL;
            }

            if (out_prev)
                *out_prev = prev;
            return node;
        }

        prev = node;
        node = node->next;
    }

    if (out_prev)
        *out_prev = NULL;
    return NULL;
}

/*
 * 判断当前数据库是否已经接近需要扩容的时机。
 *
 * 参数：
 * - db：目标数据库
 *
 * 返回值：
 * - 1：建议扩容
 * - 0：暂时不需要
 *
 * 我们这里采用一个非常朴素的判断标准：
 * - 如果下一次插入后，元素个数会超过桶数的 3/4
 * - 就认为冲突概率开始明显增加，应该扩容
 *
 * 为什么不是等桶完全塞满再扩容？
 * - 因为等冲突已经很严重再扩容，就失去提前优化查找路径的意义了
 *
 * 为什么不是直接用更复杂的负载因子策略？
 * - 因为这一章的目标是把“最小可用 rehash”讲清楚
 * - 先不要把逻辑写得过于复杂
 */
static int db_should_rehash_before_insert(const Db *db)
{
    if (db->bucket_count >= DB_MAX_BUCKET_COUNT)
        return 0;

    return (db->size + 1) > (db->bucket_count * 3 / 4);
}

/*
 * 计算下一次扩容后应当启用多少个桶。
 *
 * 参数：
 * - current_bucket_count：当前启用桶数
 *
 * 返回值：
 * - 一个新的桶数，通常是翻倍
 *
 * 为什么要单独写成函数？
 * - 因为“下一个桶数怎么选”本身也是扩容策略的一部分
 * - 单独写出来后，更方便后续章节调整策略
 */
static size_t db_next_bucket_count(size_t current_bucket_count)
{
    size_t next = current_bucket_count * 2;
    if (next > DB_MAX_BUCKET_COUNT)
        next = DB_MAX_BUCKET_COUNT;
    return next;
}

/*
 * 把数据库里的所有节点，按照“新的桶数量”重新分布一次。
 *
 * 参数：
 * - db：目标数据库
 * - new_bucket_count：扩容后希望启用的桶数量
 *
 * 返回值：
 * - 无
 *
 * 数据流：
 * 1. 先准备一份新的桶头数组视图
 * 2. 遍历旧桶里的所有节点
 * 3. 对每个节点重新计算它在新桶数组中的位置
 * 4. 再把节点重新挂进新桶链
 * 5. 最后更新 db->bucket_count
 *
 * 易错点：
 * - rehash 不是重新创建所有节点
 * - 节点本身仍然复用原来的内存，只是 next 指针和桶位置改变了
 * - 如果不提前保存 next，节点重新挂链时就会把原链表走丢
 */
static void db_rehash(Db *db, size_t new_bucket_count)
{
    DbNode *new_buckets[DB_MAX_BUCKET_COUNT];
    memset(new_buckets, 0, sizeof(new_buckets));

    for (size_t i = 0; i < db->bucket_count; i++) {
        DbNode *node = db->buckets[i];
        while (node) {
            DbNode *next = node->next;
            size_t new_idx = db_bucket_index_with_count(node->key,
                                                        node->key_len,
                                                        new_bucket_count);

            /*
             * 这里继续使用头插法挂到新桶链。
             * 重点不是保持原顺序，而是把节点稳定迁移到正确的新桶。
             */
            node->next = new_buckets[new_idx];
            new_buckets[new_idx] = node;
            node = next;
        }
    }

    memset(db->buckets, 0, sizeof(db->buckets));
    for (size_t i = 0; i < new_bucket_count; i++) {
        db->buckets[i] = new_buckets[i];
    }

    db->bucket_count = new_bucket_count;
}

/*
 * 复制一段字节数据到堆上。
 *
 * 参数：
 * - src：源字节序列
 * - len：要复制的字节数
 *
 * 返回值：
 * - 成功：返回新申请的堆内存首地址
 * - 失败：返回 NULL
 *
 * 为什么不直接保存外部传进来的指针？
 * - 因为请求缓冲区在命令处理结束后就会释放
 * - 如果数据库只记住原指针，后面访问到的就会是悬空内存
 *
 * 所以数据库必须复制出自己的副本，独立持有它。
 */
static uint8_t *db_dup_bytes(const uint8_t *src, uint32_t len)
{
    if (len == 0) {
        /*
         * 即使长度为 0，我们仍然返回一块可释放的最小内存，
         * 这样调用方在 free 时不需要额外区分。
         */
        uint8_t *buf = (uint8_t *)malloc(1);
        return buf;
    }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf)
        return NULL;

    memcpy(buf, src, len);
    return buf;
}

/*
 * 释放一个哈希节点及其内部持有的堆内存。
 *
 * 参数：
 * - node：要释放的节点
 *
 * 返回值：
 * - 无
 *
 * 为什么要把这段逻辑单独提成函数？
 * - 因为“释放节点内部 key/value，再释放节点本身”这个顺序
 *   在 db_free 和 db_del 里都会重复出现
 * - 抽出来以后，可以避免在多个地方重复写相同释放代码
 * - 也能让“节点生命周期管理”这件事更集中、更好理解
 *
 * 易错点：
 * - 不能只 free(node)，否则 key 和 value 会泄漏
 * - 也不能先 free(node) 再访问 node->key / node->val
 */
static void db_free_node(DbNode *node)
{
    free(node->key);
    free(node->val);
    free(node);
}

/*
 * 把数据库重置成“空库”状态。
 *
 * 参数：
 * - db：要初始化的数据库对象
 *
 * 返回值：
 * - 无
 *
 * 为什么这里直接 memset 整个结构体？
 * - 因为当前 Db 结构体里只有“桶数组 + 计数”
 * - 全部清零后，所有桶头指针都会变成 NULL
 *
 * 另外，这里还要手动把 bucket_count 设成初始值。
 * 原因是：
 * - “所有桶都为空”不代表“所有桶都在使用”
 * - 当前激活桶数本身也是数据库运行状态的一部分
 */
void db_init(Db *db)
{
    memset(db, 0, sizeof(*db));
    db->bucket_count = DB_INITIAL_BUCKET_COUNT;
}

/*
 * 释放数据库内部持有的所有堆内存。
 *
 * 参数：
 * - db：要释放的数据库对象
 *
 * 返回值：
 * - 无
 *
 * 数据流：
 * - 从当前正在使用的每个桶开始遍历
 * - 沿着桶链把所有节点逐个释放
 * - 最后恢复到“空库 + 初始桶数”的状态
 *
 * 易错点：
 * - 只把 size 设成 0 不够，因为那样会泄漏所有节点和它们持有的数据
 */
void db_free(Db *db)
{
    for (size_t i = 0; i < db->bucket_count; i++) {
        DbNode *node = db->buckets[i];
        while (node) {
            DbNode *next = node->next;
            db_free_node(node);
            node = next;
        }

        db->buckets[i] = NULL;
    }

    memset(db->buckets, 0, sizeof(db->buckets));
    db->size = 0;
    db->bucket_count = DB_INITIAL_BUCKET_COUNT;
}

/*
 * 执行“写入键值对”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要写入的键
 * - val / val_len：要写入的值
 *
 * 返回值：
 * - DB_STATUS_OK：成功
 * - DB_STATUS_FULL：数据库容量已满，且这是一个新 key
 * - DB_STATUS_OOM：复制 key 或 value 时申请内存失败
 *
 * 这一章这里没有改变写入接口名字。
 *
 * 它延续了之前已经建立好的两件事：
 * - 先根据当前活跃桶数决定写入位置
 * - 如果负载偏高，会先触发 rehash，再继续写入
 *
 * 这一章和过期时间有关的新增点在于：
 * - 如果旧 key 已经过期，那么 SET 会把它当成不存在
 * - 如果覆盖一个仍然有效的 key，当前会顺便清除它原来的过期时间
 */
DbStatus db_set(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t *val,
                uint32_t val_len)
{
    if (db_should_rehash_before_insert(db)) {
        size_t new_bucket_count = db_next_bucket_count(db->bucket_count);
        db_rehash(db, new_bucket_count);
    }

    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, NULL);

    if (node) {
        /*
         * 已存在这个 key：覆盖旧值。
         *
         * 数据流：
         * - 先在同一个桶链里找到旧节点
         * - 再只替换它的 value
         *
         * 为什么这里不需要重新复制 key？
         * - 因为 key 没变
         * - 原来的 key 副本仍然是有效的
         */
        uint8_t *new_val = db_dup_bytes(val, val_len);
        if (!new_val)
            return DB_STATUS_OOM;

        free(node->val);
        node->val = new_val;
        node->val_len = val_len;
        node->has_expire = false;
        node->expire_at_ms = 0;
        return DB_STATUS_OK;
    }

    if (db->size >= DB_MAX_KV) {
        return DB_STATUS_FULL;
    }

    /*
     * 新 key：需要同时准备 key 副本、value 副本和新节点对象。
     *
     * 为什么要分开准备这些对象？
     * - 因为数据库长期持有 key/value，所以必须复制出独立副本
     * - 链表节点本身也需要单独分配空间
     * - 这样数据库才能独立持有自己的完整记录
     */
    uint8_t *new_key = db_dup_bytes(key, key_len);
    if (!new_key) {
        return DB_STATUS_OOM;
    }

    uint8_t *new_val = db_dup_bytes(val, val_len);
    if (!new_val) {
        free(new_key);
        return DB_STATUS_OOM;
    }

    DbNode *new_node = (DbNode *)malloc(sizeof(DbNode));
    if (!new_node) {
        free(new_key);
        free(new_val);
        return DB_STATUS_OOM;
    }

    new_node->key = new_key;
    new_node->key_len = key_len;
    new_node->val = new_val;
    new_node->val_len = val_len;
    new_node->has_expire = false;
    new_node->expire_at_ms = 0;

    /*
     * 头插法挂到桶链前面。
     *
     * 为什么这里选头插法？
     * - 写法最简单
     * - 不需要走到链表尾部
     * - 对当前教学版本来说足够清楚
     */
    new_node->next = db->buckets[bucket_idx];
    db->buckets[bucket_idx] = new_node;
    db->size++;

    return DB_STATUS_OK;
}

/*
 * 执行“读取键值对”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要查找的键
 * - out_val / out_len：用于带出查找到的值
 *
 * 返回值：
 * - DB_STATUS_OK：找到
 * - DB_STATUS_NOT_FOUND：没找到
 *
 * 数据流：
 * - 先算出目标 key 属于当前激活桶中的哪个桶
 * - 再只在那个桶链里查找
 * - 找到后把内部 value 指针和长度返回给调用者
 */
DbStatus db_get(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t **out_val,
                uint32_t *out_len)
{
    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, NULL);
    if (!node)
        return DB_STATUS_NOT_FOUND;

    *out_val = node->val;
    *out_len = node->val_len;
    return DB_STATUS_OK;
}

/*
 * 执行“删除键值对”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要删除的键
 *
 * 返回值：
 * - DB_STATUS_OK：成功删除
 * - DB_STATUS_NOT_FOUND：没找到这个 key
 *
 * 数据流：
 * 1. 先根据 key 算出它所在的桶
 * 2. 从这个桶的链表头开始向后扫描
 * 3. 一边扫描，一边记住“前一个节点是谁”
 * 4. 找到目标节点后，把它从链表中摘掉
 * 5. 释放节点内存，并把数据库元素个数减一
 *
 * 为什么这里不用更花哨的技巧，而是老老实实维护 prev 和 node？
 * - 因为这一章的重点是把“删除链表节点”的过程讲清楚
 * - prev/node 这种写法虽然略长，但对初学者最直观
 *
 * 易错点：
 * - 如果要删除的是桶头节点，那么没有 prev，要直接改桶头指针
 * - 如果删除的是中间节点，才是 prev->next = node->next
 * - 摘链后要立刻保存好 next 关系，避免丢链
 */
DbStatus db_del(Db *db, const uint8_t *key, uint32_t key_len)
{
    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *prev = NULL;
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, &prev);
    if (!node)
        return DB_STATUS_NOT_FOUND;

    db_unlink_and_free_node(db, bucket_idx, prev, node);
    return DB_STATUS_OK;
}

/*
 * 执行“判断 key 是否存在”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：要查询的键
 *
 * 返回值：
 * - DB_STATUS_OK：key 存在
 * - DB_STATUS_NOT_FOUND：key 不存在
 *
 * 调用时机：
 * - 当命令层收到 EXISTS key 时，会调用这里
 *
 * 执行流程概览：
 * 1. 根据 key 先算出它所在的桶
 * 2. 只在该桶链上做精确匹配
 * 3. 找到节点就返回 OK
 * 4. 没找到就返回 NOT_FOUND
 *
 * 为什么这里不直接调用 db_get？
 * - 因为 EXISTS 不需要把 value 带出去
 * - 它只关心“有没有这个 key”
 * - 直接复用更底层的查找逻辑，会比先 db_get 再忽略 value 更直观
 */
DbStatus db_exists(Db *db, const uint8_t *key, uint32_t key_len)
{
    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, NULL);
    if (node)
        return DB_STATUS_OK;

    return DB_STATUS_NOT_FOUND;
}

/*
 * 执行“设置过期时间”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：目标键
 * - ttl_ms：多少毫秒后过期
 *
 * 返回值：
 * - DB_STATUS_OK：成功设置过期时间
 * - DB_STATUS_NOT_FOUND：key 不存在
 *
 * 数据流：
 * 1. 先找到目标 key 当前所在的桶
 * 2. 再查这个 key 目前是否仍然有效
 * 3. 如果 key 不存在，直接返回 NOT_FOUND
 * 4. 如果 ttl_ms <= 0，就把它立即删掉
 * 5. 否则记录“绝对过期时刻”
 */
DbStatus db_expire(Db *db,
                   const uint8_t *key,
                   uint32_t key_len,
                   int64_t ttl_ms)
{
    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *prev = NULL;
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, &prev);
    if (!node)
        return DB_STATUS_NOT_FOUND;

    if (ttl_ms <= 0) {
        db_unlink_and_free_node(db, bucket_idx, prev, node);
        return DB_STATUS_OK;
    }

    node->has_expire = true;
    node->expire_at_ms = db_now_ms() + ttl_ms;
    return DB_STATUS_OK;
}

/*
 * 执行“查询剩余过期时间”操作。
 *
 * 参数：
 * - db：目标数据库
 * - key / key_len：目标键
 * - out_ttl_ms：返回剩余毫秒数
 *
 * 返回值：
 * - DB_STATUS_OK：成功返回剩余时间
 * - DB_STATUS_NO_EXPIRE：这个 key 当前没有设置过期时间
 * - DB_STATUS_NOT_FOUND：key 不存在，或者已经过期
 *
 * 易错点：
 * - key 在查 TTL 的这一刻也可能已经过期
 * - 所以不能只看 has_expire，还要再和当前时间比较一次
 */
DbStatus db_ttl(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                int64_t *out_ttl_ms)
{
    size_t bucket_idx = db_bucket_index_with_count(key, key_len, db->bucket_count);
    DbNode *prev = NULL;
    DbNode *node = db_find_live_node(db, bucket_idx, key, key_len, &prev);
    if (!node)
        return DB_STATUS_NOT_FOUND;

    if (!node->has_expire)
        return DB_STATUS_NO_EXPIRE;

    int64_t now_ms = db_now_ms();
    int64_t remain = node->expire_at_ms - now_ms;
    if (remain <= 0) {
        db_unlink_and_free_node(db, bucket_idx, prev, node);
        return DB_STATUS_NOT_FOUND;
    }

    *out_ttl_ms = remain;
    return DB_STATUS_OK;
}
