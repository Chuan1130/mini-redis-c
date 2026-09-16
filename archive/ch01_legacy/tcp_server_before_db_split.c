/*
 * 这是第 ch01 章之前的旧实现。
 *
 * 当时它解决的问题：
 * - 用一个文件把 TCP 服务端、请求解析、命令执行、内嵌内存 KV 串起来
 * - 先把最小的 SET/GET 闭环跑通
 *
 * 为什么后来被替换：
 * - 存储层直接写在 tcp_server.c 里，职责开始混杂
 * - SET 只是追加，没有正确覆盖已有 key
 * - 继续在主文件里堆存储细节，会影响后续章节阅读
 *
 * 新实现现在在哪里：
 * - 服务端主流程：src/tcp_server.c
 * - 独立存储层：src/db.c 和 include/db.h
 *
 * 下面保留的是“拆分存储层之前”的完整服务端文件，
 * 目的是方便学习时对照当前版本和旧版本的差异。
 */

/*
 * 本文件作用：
 * 实现当前项目的主服务端。
 *
 * 这个文件目前把几个主题放在了一起，方便初学阶段沿着一条主线看完整数据流：
 * 1. TCP 监听与 accept
 * 2. poll 事件循环
 * 3. 连接读写状态机
 * 4. 请求解析后的命令执行
 * 5. 极简内存 KV 存储
 *
 * 这样做的好处是：
 * 你能从一个文件里看到“一条命令从 socket 进入，到返回结果”的全过程。
 *
 * 这样做的代价是：
 * 随着项目变大，这个文件会越来越拥挤。
 * 因此后续章节最自然的整理方向，是把存储层等逻辑逐步拆出去。
 *
 * 额外说明：
 * 文件里还保留了一段旧的 PING/ECHO 风格响应逻辑，用来保留演进痕迹。
 * 它当前不走主执行路径，后续如果开始影响阅读，适合归档到 archive/。
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

#define MAX_CONN 1024
#define MAX_MSG_SIZE (1024 * 1024)

#define MAX_KV 1024

typedef struct {
    char *key;
    char *val;
} Entry;

/*
 * 当前最小“数据库”：
 * 用固定数组保存键值对。
 *
 * 这只是教学起步版，目的是先把命令执行链路打通。
 * 它还不是一个真正高效、完整的数据结构层。
 */
static Entry g_data[MAX_KV];
static size_t g_data_count = 0;


typedef enum {
    /* 正在读取请求头的 4 字节长度。 */
    ST_READ_LEN,
    /* 已经知道 body 长度，正在读取完整请求体。 */
    ST_READ_BODY,
    /* 请求已经处理完，正在把响应写回客户端。 */
    ST_WRITE_RESP
} ConnState;

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

#define RES_OK  0
#define RES_ERR 1

static int do_request(const Args *args,
                      uint32_t *status,
                      const uint8_t **resp_data,
                      uint32_t *resp_len) 
{
    /*
     * 这是当前最小命令执行层。
     *
     * 输入：
     * - args：已经由 parse_req 解析好的参数数组
     *
     * 输出：
     * - status：成功或失败
     * - resp_data/resp_len：响应数据内容
     *
     * 当前只支持 SET 和 GET，目的是先证明“协议解析 -> 命令执行 -> 响应返回”
     * 这一整条链路已经跑通。
     */
    if (args->argc == 0) return -1;

    /* 第 0 个参数总是命令名，例如 "get" 或 "set"。 */
    const char *cmd = (const char *)args->argv[0];
    uint32_t cmd_len = args->lens[0];

    /* SET key value */
    if (cmd_len == 3 && strncasecmp(cmd, "set", 3) == 0) {
        if (args->argc != 3) goto err;
        
        const char *key = (const char *)args->argv[1];
        const char *val = (const char *)args->argv[2];
        
        /*
         * 当前阶段先用最简单的策略：直接追加。
         * 这也是下一章最值得继续改进的地方之一。
         */
        g_data[g_data_count].key = strndup(key, args->lens[1]);
        g_data[g_data_count].val = strndup(val, args->lens[2]);
        g_data_count++;

        *status = RES_OK;
        *resp_data = (const uint8_t *)"OK";
        *resp_len = 2;
    } 
    /* GET key */
    else if (cmd_len == 3 && strncasecmp(cmd, "get", 3) == 0) {
        if (args->argc != 2) goto err;
        
        const char *key = (const char *)args->argv[1];
        /* 当前查找方式是线性扫描，简单但不高效。 */
        for (size_t i = 0; i < g_data_count; i++) {
            if (strncmp(g_data[i].key, key, args->lens[1]) == 0) {
                *status = RES_OK;
                *resp_data = (const uint8_t *)g_data[i].val;
                *resp_len = strlen(g_data[i].val);
                return 0;
            }
        }
        *status = RES_ERR; // Key 不存在
    }
    else {
        *status = RES_ERR;
    }

    return 0;

err:
    *status = RES_ERR;
    return 0;
}

static void conn_free(Conn *c) {
    if (!c) return;

    /* 连接结束时，把这条连接占用的资源全部释放掉。 */
    if (c->fd >= 0) close(c->fd);
    free(c->body);
    free(c->wbuf);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

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
                    uint8_t *out = make_resp(RES_ERR, NULL, 0, &out_len);

                    c->wbuf = out;
                    c->wlen = out_len;
                    c->wsent = 0;
                    c->st = ST_WRITE_RESP;

                } else {
                    /* 请求解析成功后，进入命令执行阶段。 */
                    uint32_t status = 0;
                    const uint8_t *resp_data = NULL;
                    uint32_t resp_len = 0;

                    if (do_request(&args, &status, &resp_data, &resp_len) != 0) {
                        status = RES_ERR;
                        resp_data = NULL;
                        resp_len = 0;
                    }

                    uint32_t out_len = 0;
                    uint8_t *out = make_resp(status,
                                            resp_data,
                                            resp_len,
                                            &out_len);

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

void die(const char *msg) {
    perror(msg);
    exit(1);
}

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
