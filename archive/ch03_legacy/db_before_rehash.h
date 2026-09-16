#pragma once

/*
 * 这是第 ch03 章之前的旧实现。
 *
 * 当时它解决的问题：
 * - 把数组版存储层升级成哈希桶 + 链表的第一版
 * - 让 SET/GET 不再依赖从头扫描整个数组
 * - 让服务端在不大改上层代码的前提下，先拥有一个基础哈希表
 *
 * 为什么后来被替换：
 * - 这一版哈希表的桶数量是固定的
 * - 数据量增大后，桶链会变长，冲突会越来越明显
 * - 它还没有“扩容 / rehash”这类更完整的工程能力
 *
 * 新实现现在在哪里：
 * - 当前对外接口：include/db.h
 * - 当前主线实现：src/db.c
 *
 * 这个旧版本保存在 archive/ch03_legacy/，
 * 方便学习时对照“固定桶数哈希表”和“支持扩容/rehash 的哈希表”。
 */

#include <stddef.h>
#include <stdint.h>

#define DB_MAX_KV 1024
#define DB_BUCKET_COUNT 256

typedef struct DbNode {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
    struct DbNode *next;
} DbNode;

typedef struct {
    DbNode *buckets[DB_BUCKET_COUNT];
    size_t size;
} Db;

typedef enum {
    DB_STATUS_OK = 0,
    DB_STATUS_NOT_FOUND = 1,
    DB_STATUS_FULL = 2,
    DB_STATUS_OOM = 3
} DbStatus;

void db_init(Db *db);
void db_free(Db *db);
DbStatus db_set(Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t *val,
                uint32_t val_len);
DbStatus db_get(const Db *db,
                const uint8_t *key,
                uint32_t key_len,
                const uint8_t **out_val,
                uint32_t *out_len);
