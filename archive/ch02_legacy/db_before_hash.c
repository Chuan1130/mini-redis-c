/*
 * 这是第 ch02 章之前的旧实现。
 *
 * 当时它解决的问题：
 * - 用独立的 db.* 模块保存键值对
 * - 让 SET 支持覆盖已有 key
 * - 让 GET 支持精确匹配 key，而不是前缀误判
 *
 * 为什么后来被替换：
 * - 它底层仍然是“固定数组 + 线性扫描”
 * - 当数据量变多时，查找成本会不断上升
 * - 这一章的主题正是把存储结构升级成更像样的哈希桶版本
 *
 * 新实现现在在哪里：
 * - 对外接口：include/db.h
 * - 当前主线实现：src/db.c
 *
 * 这个文件完整保留了“哈希升级之前”的数组版实现，
 * 方便以后回头复习时做逐行对照。
 */

#include "db_before_hash.h"

#include <stdlib.h>
#include <string.h>

static int db_key_equals(const DbEntry *entry,
                         const uint8_t *key,
                         uint32_t key_len)
{
    if (entry->key_len != key_len)
        return 0;

    if (key_len == 0)
        return 1;

    return memcmp(entry->key, key, key_len) == 0;
}

static int db_find_index(const Db *db,
                         const uint8_t *key,
                         uint32_t key_len)
{
    for (size_t i = 0; i < db->size; i++) {
        if (db_key_equals(&db->entries[i], key, key_len))
            return (int)i;
    }

    return -1;
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
    for (size_t i = 0; i < db->size; i++) {
        free(db->entries[i].key);
        free(db->entries[i].val);
        db->entries[i].key = NULL;
        db->entries[i].val = NULL;
        db->entries[i].key_len = 0;
        db->entries[i].val_len = 0;
    }

    db->size = 0;
}

DbStatus db_set(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t *val,
                uint32_t val_len)
{
    uint8_t *new_val = db_dup_bytes(val, val_len);
    if (!new_val)
        return DB_STATUS_OOM;

    int idx = db_find_index(db, key, key_len);
    if (idx >= 0) {
        free(db->entries[idx].val);
        db->entries[idx].val = new_val;
        db->entries[idx].val_len = val_len;
        return DB_STATUS_OK;
    }

    if (db->size >= DB_MAX_KV) {
        free(new_val);
        return DB_STATUS_FULL;
    }

    uint8_t *new_key = db_dup_bytes(key, key_len);
    if (!new_key) {
        free(new_val);
        return DB_STATUS_OOM;
    }

    DbEntry *entry = &db->entries[db->size];
    entry->key = new_key;
    entry->key_len = key_len;
    entry->val = new_val;
    entry->val_len = val_len;
    db->size++;

    return DB_STATUS_OK;
}

DbStatus db_get(const Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t **out_val,
                uint32_t *out_len)
{
    int idx = db_find_index(db, key, key_len);
    if (idx < 0)
        return DB_STATUS_NOT_FOUND;

    *out_val = db->entries[idx].val;
    *out_len = db->entries[idx].val_len;
    return DB_STATUS_OK;
}
