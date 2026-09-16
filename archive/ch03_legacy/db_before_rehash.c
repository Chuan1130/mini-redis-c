/*
 * 这是第 ch03 章之前的旧实现。
 *
 * 当时它解决的问题：
 * - 提供哈希桶 + 链表的第一版存储层
 * - 让查找不再依赖数组线性扫描
 * - 先把“哈希冲突用链表处理”这件事讲清楚
 *
 * 为什么后来被替换：
 * - 它的桶数量固定不变
 * - 当数据不断增加时，桶链会变长
 * - 这会让哈希表逐渐退化，缺少更完整的工程能力
 *
 * 新实现现在在哪里：
 * - 当前对外接口：include/db.h
 * - 当前主线实现：src/db.c
 *
 * 这个文件完整保留了“rehash 之前”的哈希表第一版实现，
 * 用于学习时和 ch03 的扩容版做对照。
 */

#include "db_before_rehash.h"

#include <stdlib.h>
#include <string.h>

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

static uint32_t db_hash_key(const uint8_t *key, uint32_t key_len)
{
    uint32_t hash = 2166136261u;

    for (uint32_t i = 0; i < key_len; i++) {
        hash ^= key[i];
        hash *= 16777619u;
    }

    return hash;
}

static size_t db_bucket_index(const uint8_t *key, uint32_t key_len)
{
    return (size_t)(db_hash_key(key, key_len) % DB_BUCKET_COUNT);
}

static DbNode *db_find_node_in_bucket(const Db *db,
                                      size_t bucket_idx,
                                      const uint8_t *key,
                                      uint32_t key_len)
{
    DbNode *node = db->buckets[bucket_idx];
    while (node) {
        if (db_key_equals(node, key, key_len))
            return node;
        node = node->next;
    }

    return NULL;
}

static uint8_t *db_dup_bytes(const uint8_t *src, uint32_t len)
{
    if (len == 0) {
        uint8_t *buf = (uint8_t *)malloc(1);
        return buf;
    }

    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf)
        return NULL;

    memcpy(buf, src, len);
    return buf;
}

void db_init(Db *db)
{
    memset(db, 0, sizeof(*db));
}

void db_free(Db *db)
{
    for (size_t i = 0; i < DB_BUCKET_COUNT; i++) {
        DbNode *node = db->buckets[i];
        while (node) {
            DbNode *next = node->next;
            free(node->key);
            free(node->val);
            free(node);
            node = next;
        }

        db->buckets[i] = NULL;
    }

    db->size = 0;
}

DbStatus db_set(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t *val,
                uint32_t val_len)
{
    size_t bucket_idx = db_bucket_index(key, key_len);
    DbNode *node = db_find_node_in_bucket(db, bucket_idx, key, key_len);

    if (node) {
        uint8_t *new_val = db_dup_bytes(val, val_len);
        if (!new_val)
            return DB_STATUS_OOM;

        free(node->val);
        node->val = new_val;
        node->val_len = val_len;
        return DB_STATUS_OK;
    }

    if (db->size >= DB_MAX_KV) {
        return DB_STATUS_FULL;
    }

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
    new_node->next = db->buckets[bucket_idx];
    db->buckets[bucket_idx] = new_node;
    db->size++;

    return DB_STATUS_OK;
}

DbStatus db_get(const Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t **out_val,
                uint32_t *out_len)
{
    size_t bucket_idx = db_bucket_index(key, key_len);
    DbNode *node = db_find_node_in_bucket(db, bucket_idx, key, key_len);
    if (!node)
        return DB_STATUS_NOT_FOUND;

    *out_val = node->val;
    *out_len = node->val_len;
    return DB_STATUS_OK;
}
