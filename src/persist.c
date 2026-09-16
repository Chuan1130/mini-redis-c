/*
 * 本文件属于哪一章：
 * - ch08：围绕字符串 KV 主线，加入最小快照持久化
 *
 * 本文件负责什么：
 * - 把当前数据库保存到本地二进制快照文件
 * - 在服务端启动时从快照文件恢复数据库
 * - 保留 key、value 和过期时间信息
 *
 * 本文件和其他文件如何配合：
 * - include/persist.h：声明本文件实现的接口
 * - src/tcp_server.c：启动时调用 persist_load，收到 SAVE 时调用 persist_save
 * - db.*：提供当前数据库结构和写入/设置过期时间接口
 *
 * 这一章最重要的学习点：
 * - 内存数据库如果不落盘，服务端一退出数据就全没了
 * - 所以“把内存状态序列化到文件”是数据库非常核心的一步
 * - 当前先做最小快照，不做复杂 AOF 或后台自动保存
 */

#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 当前快照文件的魔数。
 *
 * 为什么要写魔数？
 * - 因为加载文件时，先验证“这是不是我们自己格式的快照”
 * - 如果魔数不对，至少可以尽早判断文件格式不匹配
 */
static const uint8_t k_snapshot_magic[4] = { 'M', 'Y', 'R', '1' };

static int64_t persist_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int persist_node_is_expired(const DbNode *node, int64_t now_ms)
{
    return node->has_expire && node->expire_at_ms <= now_ms;
}

/*
 * 向文件写一个 4 字节无符号整数，按大端序保存。
 */
static int write_u32_be(FILE *fp, uint32_t value)
{
    uint8_t buf[4];
    buf[0] = (uint8_t)((value >> 24) & 0xff);
    buf[1] = (uint8_t)((value >> 16) & 0xff);
    buf[2] = (uint8_t)((value >> 8) & 0xff);
    buf[3] = (uint8_t)(value & 0xff);
    return fwrite(buf, 1, 4, fp) == 4 ? 0 : -1;
}

/*
 * 向文件写一个 8 字节有符号整数，按大端序保存。
 */
static int write_i64_be(FILE *fp, int64_t value)
{
    uint64_t v = (uint64_t)value;
    uint8_t buf[8];
    for (int i = 7; i >= 0; i--) {
        buf[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
    return fwrite(buf, 1, 8, fp) == 8 ? 0 : -1;
}

/*
 * 从文件读一个 4 字节无符号整数，按大端序解释。
 */
static int read_u32_be(FILE *fp, uint32_t *out)
{
    uint8_t buf[4];
    if (fread(buf, 1, 4, fp) != 4)
        return -1;

    *out = ((uint32_t)buf[0] << 24) |
           ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) |
           (uint32_t)buf[3];
    return 0;
}

/*
 * 从文件读一个 8 字节有符号整数，按大端序解释。
 */
static int read_i64_be(FILE *fp, int64_t *out)
{
    uint8_t buf[8];
    if (fread(buf, 1, 8, fp) != 8)
        return -1;

    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | buf[i];
    }
    *out = (int64_t)v;
    return 0;
}

/*
 * 把一段原始字节写入文件。
 */
static int write_bytes(FILE *fp, const uint8_t *data, uint32_t len)
{
    return fwrite(data, 1, len, fp) == len ? 0 : -1;
}

/*
 * 从文件中读一段字节到新分配缓冲区。
 *
 * 参数：
 * - fp：输入文件
 * - len：要读取的长度
 * - out：返回新分配的缓冲区
 *
 * 调用方负责 free(*out)。
 */
static int read_alloc_bytes(FILE *fp, uint32_t len, uint8_t **out)
{
    uint8_t *buf = (uint8_t *)malloc(len == 0 ? 1 : len);
    if (!buf)
        return -1;

    if (len > 0 && fread(buf, 1, len, fp) != len) {
        free(buf);
        return -1;
    }

    *out = buf;
    return 0;
}

/*
 * 统计当前数据库里“仍然有效、应该写入快照”的节点数量。
 */
static uint32_t persist_live_entry_count(const Db *db)
{
    uint32_t count = 0;
    int64_t now_ms = persist_now_ms();

    for (size_t i = 0; i < db->bucket_count; i++) {
        for (DbNode *node = db->buckets[i]; node; node = node->next) {
            if (!persist_node_is_expired(node, now_ms))
                count++;
        }
    }

    return count;
}

int persist_save(const Db *db, const char *path)
{
    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;

    if (fwrite(k_snapshot_magic, 1, 4, fp) != 4) {
        fclose(fp);
        return -1;
    }

    uint32_t count = persist_live_entry_count(db);
    if (write_u32_be(fp, count) != 0) {
        fclose(fp);
        return -1;
    }

    int64_t now_ms = persist_now_ms();

    for (size_t i = 0; i < db->bucket_count; i++) {
        for (DbNode *node = db->buckets[i]; node; node = node->next) {
            if (persist_node_is_expired(node, now_ms))
                continue;

            if (write_u32_be(fp, node->key_len) != 0 ||
                write_bytes(fp, node->key, node->key_len) != 0 ||
                write_u32_be(fp, node->val_len) != 0 ||
                write_bytes(fp, node->val, node->val_len) != 0 ||
                write_u32_be(fp, node->has_expire ? 1u : 0u) != 0) {
                fclose(fp);
                return -1;
            }

            if (node->has_expire && write_i64_be(fp, node->expire_at_ms) != 0) {
                fclose(fp);
                return -1;
            }
        }
    }

    if (fclose(fp) != 0)
        return -1;

    return 0;
}

int persist_load(Db *db, const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return 1;

    uint8_t magic[4];
    if (fread(magic, 1, 4, fp) != 4 || memcmp(magic, k_snapshot_magic, 4) != 0) {
        fclose(fp);
        return -1;
    }

    uint32_t count = 0;
    if (read_u32_be(fp, &count) != 0) {
        fclose(fp);
        return -1;
    }

    Db tmp;
    db_init(&tmp);

    int64_t now_ms = persist_now_ms();

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key_len = 0;
        uint32_t val_len = 0;
        uint32_t has_expire = 0;
        int64_t expire_at_ms = 0;
        uint8_t *key = NULL;
        uint8_t *val = NULL;

        if (read_u32_be(fp, &key_len) != 0 ||
            read_alloc_bytes(fp, key_len, &key) != 0 ||
            read_u32_be(fp, &val_len) != 0 ||
            read_alloc_bytes(fp, val_len, &val) != 0 ||
            read_u32_be(fp, &has_expire) != 0) {
            free(key);
            free(val);
            db_free(&tmp);
            fclose(fp);
            return -1;
        }

        if (has_expire) {
            if (read_i64_be(fp, &expire_at_ms) != 0) {
                free(key);
                free(val);
                db_free(&tmp);
                fclose(fp);
                return -1;
            }
        }

        if (!has_expire || expire_at_ms > now_ms) {
            if (db_set(&tmp, key, key_len, val, val_len) != DB_STATUS_OK) {
                free(key);
                free(val);
                db_free(&tmp);
                fclose(fp);
                return -1;
            }

            if (has_expire) {
                int64_t ttl_ms = expire_at_ms - now_ms;
                if (db_expire(&tmp, key, key_len, ttl_ms) != DB_STATUS_OK) {
                    free(key);
                    free(val);
                    db_free(&tmp);
                    fclose(fp);
                    return -1;
                }
            }
        }

        free(key);
        free(val);
    }

    if (fclose(fp) != 0) {
        db_free(&tmp);
        return -1;
    }

    db_free(db);
    *db = tmp;
    return 0;
}
