/*
 * 本文件属于哪一章：
 * - ch08：围绕字符串 KV 主线，加入最小快照持久化
 *
 * 本文件负责什么：
 * - 作为当前项目的主服务端入口
 * - 负责 TCP 监听、accept、poll 事件循环
 * - 负责把完整请求读出来并交给命令层处理
 * - 在命令层中调用 db.* 完成真正的数据读写
 *
 * 它和其他文件如何配合：
 * - src/proto.c / src/codec.c：定义和实现请求、响应格式
 * - src/db.c：实现当前章节带扩容 / rehash、删除、存在性判断和过期时间的哈希表数据库
 * - src/persist.c：实现当前章节的快照保存和加载
 * - src/nb.c：把 socket 设成非阻塞，保证 poll 模型可以正常工作
 *
 * 为什么这一章还允许它同时承担很多职责？
 * - 因为这个项目是教学项目
 * - 对初学者来说，先在一个文件里看清“命令从进来到出去”的完整路径更重要
 *
 * 额外说明：
 * - 文件里还保留了一段旧的 PING/ECHO 风格响应逻辑，用来保留演进痕迹
 * - 它当前不走主执行路径，后续如果开始影响阅读，适合归档到 archive/
 */

#include <poll.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdio.h>

#include "nb.h"
#include "codec.h"
#include "db.h"
#include "persist.h"

#define MAX_CONN 1024
#define MAX_MSG_SIZE (1024 * 1024)

/*
 * g_db 是整个服务端共享的一份内存数据库。
 *
 * 为什么这里用一个全局对象？
 * - 因为当前项目还是单线程、单进程的最小版本
 * - 所有客户端连接共享同一份数据库状态，符合 Redis “一个服务端维护一份数据库”的直觉
 *
 * 这和上一章的区别：
 * - 上一章是 g_data / g_data_count 这类裸数组
 * - ch02 开始，服务端只持有一个 Db 对象
 * - 到 ch03，这个 Db 对象内部已经支持最小可用的扩容 / rehash
 * - 到 ch04，这个 Db 对象又继续学会了删除节点
 * - 到 ch05，这个 Db 对象已经能直接判断 key 是否存在
 * - 到 ch06，命令层又开始返回更清楚的响应类型
 * - 到 ch07，Db 节点又开始携带过期时间
 * - 到 ch08，服务端启动时又会尝试从快照文件恢复数据库
 */
static Db g_db;

typedef enum {
    /* 正在读取请求头的 4 字节长度。 */
    ST_READ_LEN,
    /* 已经知道 body 长度，正在读取完整请求体。 */
    ST_READ_BODY,
    /* 请求已经处理完，正在把响应写回客户端。 */
    ST_WRITE_RESP
} ConnState;

/*
 * Conn 表示“一个已经建立的客户端连接”。
 *
 * 你可以把它理解成：服务端为每个客户端保存的一份“会话现场”。
 *
 * 字段分两部分：
 * - 读请求状态：当前读到了哪里
 * - 写响应状态：当前写到了哪里
 *
 * 为什么需要这些字段？
 * - 因为非阻塞 I/O 下，一次 read/write 不一定就把整条消息处理完
 * - 所以必须把“处理进度”记住，等下次 poll 告诉我们可读/可写时继续
 */
typedef struct {
    int fd;
    ConnState st;

    /* 下面这几项记录“读请求”的进度。 */
    uint8_t lenbuf[4];
    size_t len_got;       /* 当前已经读到了多少个长度字节，范围 0..4 */
    uint32_t body_len;
    uint8_t *body;
    size_t body_got;

    /* 下面这几项记录“写响应”的进度。 */
    uint8_t *wbuf;
    size_t wlen;
    size_t wsent;
} Conn;

/*
 * 把一个“不是以 '\0' 结尾的参数字节串”解析成有符号整数。
 *
 * 参数：
 * - data / len：参数原始字节
 * - out：返回解析出的整数
 *
 * 返回值：
 * - 0：成功
 * - -1：失败，说明参数不是合法整数
 *
 * 为什么需要这个辅助函数？
 * - 因为 Args 里的参数只是“字节视图”，不是标准 C 字符串
 * - strtoll 这类库函数要求输入必须以 '\0' 结尾
 * - 所以这里要先复制出一份临时字符串再解析
 */
static int arg_to_i64(const uint8_t *data, uint32_t len, int64_t *out)
{
    char *buf = (char *)malloc(len + 1);
    if (!buf)
        return -1;

    memcpy(buf, data, len);
    buf[len] = '\0';

    char *end = NULL;
    long long v = strtoll(buf, &end, 10);
    if (end == buf || *end != '\0') {
        free(buf);
        return -1;
    }

    *out = (int64_t)v;
    free(buf);
    return 0;
}

/*
 * 处理一条已经解析好的请求。
 *
 * 参数：
 * - args：由 parse_req 解析出来的命令参数
 * - out：输出响应语义对象
 *
 * 返回值：
 * - 0：命令层本身执行完毕，最终结果保存在 out 中
 * - -1：命令层出现了意料外的问题
 *
 * 数据流：
 * 1. 先看 argv[0] 是什么命令
 * 2. 如果是 SET，就调用 db_set
 * 3. 如果是 GET，就调用 db_get
 * 4. 如果是 DEL，就调用 db_del
 * 5. 如果是 EXISTS，就调用 db_exists
 * 6. 如果是 EXPIRE / TTL，就调用过期时间相关接口
 * 7. 如果是 SAVE，就调用持久化接口
 * 8. 把数据库返回的结果转换成当前协议里的 RespType
 *
 * 易错点：
 * - args 里的指针都指向请求缓冲区
 * - 命令层自己不能长期保存这些指针
 * - 真正需要长期保存时，必须由 db_set 复制到数据库内部
 */
static int do_request(const Args *args,
                      Resp *out)
{
    /*
     * 这是当前最小命令执行层。
     *
     * 输入：
     * - args：已经由 parse_req 解析好的参数数组
     *
     * 输出：
     * - out：本次命令最终应该返回什么类型的结果
     *
     * 到这一章为止，当前最小命令集已经扩成：
     * - SET / GET / DEL / EXISTS
     * - EXPIRE / TTL / SAVE
     *
     * 这样做的意义是：
     * - SET 更像“简单成功”
     * - GET 更像“字符串结果”或“空结果”
     * - DEL 更像“整数结果”
     * - EXISTS 也更像“整数结果”
     * - EXPIRE / TTL 则开始让 key 带上“时间语义”
     * - SAVE 则开始让数据库状态可以落到本地文件
     *
     * 从这一章开始，数据库第一次不再只关心“有没有这个 key”，
     * 还开始关心“它现在是不是已经过期”。
     */
    if (args->argc == 0) return -1;

    /* 第 0 个参数总是命令名，例如 "get" 或 "set"。 */
    const char *cmd = (const char *)args->argv[0];
    uint32_t cmd_len = args->lens[0];

    /*
     * 先给 out 一个明确的默认值。
     *
     * 为什么要这样做？
     * - 因为一旦后续某个分支提前返回，out 至少是一个可预期状态
     * - 对排查“忘记设置响应类型”的问题会更安全
     */
    out->type = RESP_ERR;
    out->data = NULL;
    out->len = 0;
    out->integer = 0;

    /*
     * SET key value
     *
     * 这一章这里没有改变 SET 的协议行为。
     * - tcp_server.c 仍然通过 db_set 调用存储层
     * - db_set 内部仍然可能在写入前触发 rehash
     * - 这一章的新增重点不在 SET，而在后面的 EXISTS
     */
    if (cmd_len == 3 && strncasecmp(cmd, "set", 3) == 0) {
        if (args->argc != 3) goto err;

        DbStatus rc = db_set(&g_db,
                             args->argv[1],
                             args->lens[1],
                             args->argv[2],
                             args->lens[2]);

        if (rc != DB_STATUS_OK) {
            out->type = RESP_ERR;
            out->data = (const uint8_t *)"set failed";
            out->len = 10;
            return 0;
        }

        out->type = RESP_OK;
    }
    /*
     * GET key
     *
     * ch06 之后，GET 的“不存在”不再被当成 ERR，
     * 而是被明确表达成 RESP_NIL。
     *
     * 这样客户端看到它时，就能打印成更像数据库语义的“空结果”，
     * 而不是笼统的错误。
     */
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
    /*
     * DEL key
     *
     * 这是上一章已经引入的最小删除命令。
     *
     * 当前协议仍然很简单：
     * - 删除成功：返回整数 1
     * - key 不存在：返回整数 0
     *
     * 这其实比上一章更贴近 Redis 风格：
     * - DEL 更像“返回影响了几个 key”
     * - 而不是简单的 OK / ERR
     */
    else if (cmd_len == 3 && strncasecmp(cmd, "del", 3) == 0) {
        if (args->argc != 2) goto err;

        DbStatus rc = db_del(&g_db, args->argv[1], args->lens[1]);
        out->type = RESP_INT;
        out->integer = (rc == DB_STATUS_OK) ? 1 : 0;
    }
    /*
     * EXISTS key
     *
     * 这是上一章已经引入的“存在性检查”命令。
     *
     * 当前返回策略：
     * - key 存在：返回 OK + "1"
     * - key 不存在：返回 OK + "0"
     *
     * 为什么这里不把“不存在”当成 ERR？
     * - 因为 EXISTS 的语义不是“执行失败”
     * - 它本来就是在问一个二选一问题：在，还是不在
     * - 所以用 "1" / "0" 作为成功结果，对初学者更直观
     */
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
    /*
     * EXPIRE key seconds
     *
     * 这是本章新增的最小过期时间设置命令。
     *
     * 当前返回策略：
     * - 设置成功：返回整数 1
     * - key 不存在：返回整数 0
     *
     * 为什么返回整数而不是 OK/ERR？
     * - 因为它问的是“有没有真的设置到一个现有 key 上”
     * - 这和 DEL / EXISTS 的结果风格是一致的
     */
    else if (cmd_len == 6 && strncasecmp(cmd, "expire", 6) == 0) {
        if (args->argc != 3) goto err;

        int64_t seconds = 0;
        if (arg_to_i64(args->argv[2], args->lens[2], &seconds) != 0 || seconds < 0) {
            out->type = RESP_ERR;
            out->data = (const uint8_t *)"expire seconds must be >= 0";
            out->len = 27;
            return 0;
        }

        if (seconds > INT64_MAX / 1000) {
            out->type = RESP_ERR;
            out->data = (const uint8_t *)"expire value too large";
            out->len = 22;
            return 0;
        }

        DbStatus rc = db_expire(&g_db,
                                args->argv[1],
                                args->lens[1],
                                seconds * 1000);
        out->type = RESP_INT;
        out->integer = (rc == DB_STATUS_OK) ? 1 : 0;
    }
    /*
     * TTL key
     *
     * 这是本章新增的最小剩余过期时间查询命令。
     *
     * 当前返回策略尽量贴近 Redis：
     * - 返回 >= 0：还剩多少秒
     * - 返回 -1：key 存在，但没有设置过期时间
     * - 返回 -2：key 不存在
     *
     * 这里为了教学更直观，秒数采用“向上取整”方式显示。
     * 也就是说，只要还剩不到 1 秒但还没过期，仍然会显示 1。
     */
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
    /*
     * SAVE
     *
     * 这是本章新增的最小快照保存命令。
     *
     * 当前设计非常克制：
     * - 只做手动触发保存
     * - 不做后台线程
     * - 不做复杂日志
     *
     * 也就是说，这一章只先把“把当前数据库完整写到一个本地文件”走通。
     */
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
    else {
        out->type = RESP_ERR;
        out->data = (const uint8_t *)"unknown command";
        out->len = 15;
    }

    return 0;

err:
    out->type = RESP_ERR;
    out->data = (const uint8_t *)"wrong number of arguments";
    out->len = 25;
    return 0;
}

/*
 * 释放一条连接占用的资源。
 *
 * 参数：
 * - c：要释放的连接对象
 *
 * 返回值：
 * - 无
 *
 * 数据流：
 * - 关闭 socket
 * - 释放这条连接读请求和写响应时分配的堆内存
 * - 再把结构体重置成“空槽位”
 */
static void conn_free(Conn *c) {
    if (!c) return;

    /* 连接结束时，把这条连接占用的资源全部释放掉。 */
    if (c->fd >= 0) close(c->fd);
    free(c->body);
    free(c->wbuf);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

/*
 * 旧阶段遗留函数。
 *
 * 参数：
 * - c：当前连接
 * - req / req_len：旧版本里当作纯文本命令处理的请求体
 *
 * 返回值：
 * - 0：成功组装出响应
 * - -1：发生内存分配失败等错误
 *
 * 为什么本章不删它？
 * - 因为它代表了更早阶段“直接把 payload 当文本命令”的实现方式
 * - 保留下来有利于你理解项目是怎样一步步演进的
 *
 * 为什么它现在不在主路径上？
 * - 因为现在主路径已经变成：
 *   parse_req -> do_request -> make_resp
 * - 旧函数只作为学习对照，不参与当前正式流程
 */
static int conn_make_resp(Conn *c, const uint8_t *req, uint32_t req_len) {
    /*
     * 旧逻辑保留说明：
     * 这段代码属于较早阶段的实验路径，当时 payload 被当成一整条文本命令。
     * 现在主路径已经改成“结构化请求解析 + do_request 执行”，
     * 所以这个函数当前没有被主流程调用。
     *
     * 之所以先保留不删，是为了保留学习演进痕迹。
     * 如果后续它开始明显干扰阅读，适合移动到 archive/ch01_legacy/。
     */
    const char *line = (const char*)req;

    const char *payload = NULL;
    char *dyn = NULL;

    if (req_len == 4 && memcmp(line, "PING", 4) == 0) {
        payload = "+PONG";
    } else if (req_len >= 5 && memcmp(line, "ECHO ", 5) == 0) {
        const char *msg = line + 5;
        size_t outlen = 1 + strlen(msg);
        dyn = (char*)malloc(outlen + 1);
        if (!dyn) return -1;
        dyn[0] = '+';
        memcpy(dyn + 1, msg, strlen(msg));
        dyn[outlen] = 0;
        payload = dyn;
    } else {
        payload = "-ERR unknown command";
    }

    uint32_t plen = (uint32_t)strlen(payload);
    uint32_t netlen = htonl(plen);

    c->wlen = 4 + plen;
    c->wbuf = (uint8_t*)malloc(c->wlen);
    if (!c->wbuf) { free(dyn); return -1; }

    memcpy(c->wbuf, &netlen, 4);
    memcpy(c->wbuf + 4, payload, plen);
    c->wsent = 0;

    free(dyn);
    return 0;
}

/*
 * 处理“某条连接当前可读”时的逻辑。
 *
 * 参数：
 * - c：当前连接对象
 *
 * 返回值：
 * - 0：这次事件处理完成，连接仍然有效
 * - 1：对端关闭，应当回收这条连接
 * - -1：发生错误，也应当回收这条连接
 *
 * 数据流：
 * 1. 先把 4 字节长度头读完整
 * 2. 再把整个请求体读完整
 * 3. 用 parse_req 解析参数
 * 4. 用 do_request 执行命令
 * 5. 用 make_resp 组装响应
 * 6. 切换到 ST_WRITE_RESP，等待后续写事件
 */
static int conn_on_readable(Conn *c) {
    /*
     * 这个函数处理“这条连接现在可读”时该做什么。
     *
     * 整体流程：
     * 1. 先把 4 字节长度读完整
     * 2. 再把 body 读完整
     * 3. 解析请求
     * 4. 执行命令
     * 5. 生成响应并切换到写状态
     */
    for (;;) {
        if (c->st == ST_READ_LEN) {
            ssize_t r = read(c->fd, c->lenbuf + c->len_got, 4 - c->len_got);
            if (r == 0) return 1; // closed
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            c->len_got += (size_t)r;
            if (c->len_got == 4) {
                uint32_t net = 0;
                memcpy(&net, c->lenbuf, 4);
                c->body_len = ntohl(net);
                if (c->body_len > MAX_MSG_SIZE) return -1;

                /* 长度到手后，准备一块刚好能装下 body 的内存。 */
                c->body = (uint8_t*)malloc(c->body_len + 1);
                if (!c->body) return -1;
                c->body_got = 0;
                c->st = ST_READ_BODY;
            }
        }

        if (c->st == ST_READ_BODY) {
            ssize_t r = read(c->fd, c->body + c->body_got, c->body_len - c->body_got);
            if (r == 0) return 1;
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
                return -1;
            }
            c->body_got += (size_t)r;
            if (c->body_got == c->body_len) {

                Args args;

                if (parse_req(c->body, c->body_len, &args) != 0) {
                    /* 请求格式不合法时，直接返回错误响应。 */
                    uint32_t out_len = 0;
                    Resp resp = {
                        .type = RESP_ERR,
                        .data = (const uint8_t *)"bad request",
                        .len = 11,
                        .integer = 0,
                    };
                    uint8_t *out = make_resp(&resp, &out_len);
                    if (!out)
                        return -1;

                    c->wbuf = out;
                    c->wlen = out_len;
                    c->wsent = 0;
                    c->st = ST_WRITE_RESP;

                } else {
                    /* 请求解析成功后，进入命令执行阶段。 */
                    Resp resp;

                    if (do_request(&args, &resp) != 0) {
                        resp.type = RESP_ERR;
                        resp.data = (const uint8_t *)"internal error";
                        resp.len = 14;
                        resp.integer = 0;
                    }

                    uint32_t out_len = 0;
                    uint8_t *out = make_resp(&resp, &out_len);
                    if (!out)
                        return -1;

                    c->wbuf = out;
                    c->wlen = out_len;
                    c->wsent = 0;
                    c->st = ST_WRITE_RESP;
                }

                /* 这一轮请求已经处理完，读缓冲区可以释放并重置状态。 */
                free(c->body);
                c->body = NULL;
                c->len_got = 0;
                c->body_len = 0;
                c->body_got = 0;

                return 0;
            }
        }
    }
}

/*
 * 处理“某条连接当前可写”时的逻辑。
 *
 * 参数：
 * - c：当前连接对象
 *
 * 返回值：
 * - 0：这次写事件处理完成，连接仍然有效
 * - -1：发生错误，应当关闭连接
 *
 * 为什么这里也要记录写进度？
 * - 因为非阻塞 socket 一次 write 可能只写出一部分
 * - 如果不记录 wsent，下次就不知道从哪里继续写
 */
static int conn_on_writable(Conn *c) {
    /* 响应可能一次写不完，所以要记录已经写到了哪里。 */
    while (c->wsent < c->wlen) {
        ssize_t w = write(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        c->wsent += (size_t)w;
    }

    /* 一条响应写完后，回到读下一条请求的状态。 */
    free(c->wbuf);
    c->wbuf = NULL;
    c->wlen = 0;
    c->wsent = 0;
    c->st = ST_READ_LEN;
    return 0;
}

/*
 * 打印错误并直接退出进程。
 *
 * 参数：
 * - msg：当前出错阶段的提示词，比如 "socket"、"bind"
 *
 * 返回值：
 * - 无，函数内部直接 exit(1)
 *
 * 为什么这里允许简单粗暴地退出？
 * - 因为这类错误发生在服务端启动阶段，继续运行通常也没有意义
 */
void die(const char *msg) {
    perror(msg);
    exit(1);
}





/*
 * 服务端主函数。
 *
 * 数据流总览：
 * 1. 创建监听 socket
 * 2. 设置地址复用、bind、listen
 * 3. 初始化数据库和连接槽
 * 4. 如果本地已有快照文件，先尝试恢复数据库
 * 5. 把监听 socket 设成非阻塞
 * 6. 进入 poll 循环
 * 7. 有新连接就 accept
 * 8. 有读事件就收请求、执行命令
 * 9. 有写事件就回响应
 *
 * 你学习这一章时，最值得重点看的不是每个系统调用的细节，
 * 而是：
 * - 服务端主流程保持基本不变
 * - 但现在又多了一步“启动时尝试恢复历史状态”
 *
 * 这能帮助你理解一个很重要的工程概念：
 * - 只要接口设计得稳定，底层实现可以在不大改业务代码的前提下逐步升级
 */
int main(void) {
    /*
     * 先向操作系统申请一个 IPv4 + TCP 的监听 socket。
     * 可以把它理解成：先拿到一个“以后用来收连接的入口”。
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    int val = 1;
    /*
     * 打开 SO_REUSEADDR，方便服务端重启后快速重新绑定同一个端口。
     * 否则旧连接还停留在 TIME_WAIT 时，重新 bind 可能失败。
     */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val)) < 0)
        die("setsockopt");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(15001);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 监听本机所有网卡 */

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("bind");

    if (listen(fd, SOMAXCONN) < 0)
        die("listen");

    Conn conns[MAX_CONN];
    for (int i = 0; i < MAX_CONN; i++) conns[i].fd = -1;

    /* 数据库在服务端启动时初始化一次。 */
    db_init(&g_db);

    /*
     * 启动阶段尝试加载快照。
     *
     * 返回值语义：
     * - 0：成功加载
     * - 1：文件不存在，不算错误，表示当前没有历史快照
     * - -1：文件存在但加载失败，这里先打印警告并继续用空库启动
     */
    int load_rc = persist_load(&g_db, SNAPSHOT_FILE);
    if (load_rc < 0) {
        fprintf(stderr, "warning: failed to load snapshot: %s\n", SNAPSHOT_FILE);
    }

    /* 监听 socket 也要设成非阻塞，才能和 poll 配合工作。 */
    set_nonblocking(fd);

    while (1) {
        struct pollfd pfds[MAX_CONN + 1];
        int idx_map[MAX_CONN + 1]; /* pfds 下标 -> conns 下标 */
        int nfd = 0;

        /* 第 0 个位置固定放监听 socket，用来等待新连接。 */
        pfds[nfd].fd = fd;
        pfds[nfd].events = POLLIN;
        idx_map[nfd] = -1;
        nfd++;

        /* 其余位置放已经建立好的客户端连接。 */
        for (int i = 0; i < MAX_CONN; i++) {
            if (conns[i].fd < 0) continue;
            pfds[nfd].fd = conns[i].fd;
            pfds[nfd].events = (conns[i].st == ST_WRITE_RESP) ? POLLOUT : POLLIN;
            idx_map[nfd] = i;
            nfd++;
        }

        int r = poll(pfds, nfd, -1);
        if (r < 0) continue;

        /* 1. 先处理新连接接入。 */
        if (pfds[0].revents & POLLIN) {
            for (;;) {
                int cfd = accept(fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    break;
                }
                set_nonblocking(cfd);

                int placed = 0;
                /* 把新连接放进空闲槽位，并初始化它的读写状态。 */
                for (int i = 0; i < MAX_CONN; i++) {
                    if (conns[i].fd < 0) {
                        conns[i].fd = cfd;
                        conns[i].st = ST_READ_LEN;
                        conns[i].len_got = 0;
                        conns[i].body_len = 0;
                        conns[i].body = NULL;
                        conns[i].body_got = 0;
                        conns[i].wbuf = NULL;
                        conns[i].wlen = 0;
                        conns[i].wsent = 0;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(cfd); /* 连接槽满了，直接拒绝新连接 */
            }
        }

        /* 2. 再处理已经存在的连接上的读写事件。 */
        for (int k = 1; k < nfd; k++) {
            int i = idx_map[k];
            if (i < 0) continue;
            Conn *c = &conns[i];

            int dead = 0;
            if (pfds[k].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                dead = 1;
            } else if ((pfds[k].revents & POLLIN) && c->st != ST_WRITE_RESP) {
                /* 当前连接在“读阶段”，所以处理读事件。 */
                int rr = conn_on_readable(c);
                if (rr != 0) dead = 1;
            } else if ((pfds[k].revents & POLLOUT) && c->st == ST_WRITE_RESP) {
                /* 当前连接在“写阶段”，所以处理写事件。 */
                int ww = conn_on_writable(c);
                if (ww != 0) dead = 1;
            }

            /* 连接出错或关闭时，释放资源并把槽位标记为空闲。 */
            if (dead) conn_free(c);
        }
    }

    close(fd);
    return 0;
}
