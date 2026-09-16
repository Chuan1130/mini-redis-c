#pragma once

/*
 * 这是第 ch02 章之前的旧实现。
 *
 * 当时它解决的问题：
 * - 把存储层从 tcp_server.c 中独立出来
 * - 提供最小可运行的 db_init / db_set / db_get / db_free 接口
 * - 先用固定数组把 SET/GET 的行为做正确
 *
 * 为什么后来被替换：
 * - 数组实现查找依赖线性扫描
 * - 数据一多时，每次 GET/SET 都可能从头扫到尾
 * - 这会让存储层在规模稍大时明显变慢
 *
 * 新实现现在在哪里：
 * - 新的对外接口：include/db.h
 * - 新的哈希桶实现：src/db.c
 *
 * 这个旧版本保存在 archive/ch02_legacy/，
 * 是为了方便学习时对照“数组版存储层”和“哈希版存储层”的差异。
 */

#include <stddef.h>
#include <stdint.h>

#define DB_MAX_KV 1024

typedef struct {
    uint8_t *key;
    uint32_t key_len;
    uint8_t *val;
    uint32_t val_len;
} DbEntry;

typedef struct {
    DbEntry entries[DB_MAX_KV];
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
