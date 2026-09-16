/*
 * 本文件属于哪一章：
 * - ch06：整理响应语义，让客户端能更清楚地理解服务器结果
 *
 * 本文件负责什么：
 * - 实现一个最小学习用客户端
 * - 从终端读取一行命令
 * - 按当前项目请求协议编码并发送
 * - 收到响应后，按 RespType 解释并打印
 *
 * 本文件和其他文件如何配合：
 * - src/proto.c：负责最外层消息边界收发
 * - src/codec.c：负责请求编码规则和响应解析规则
 * - src/tcp_server.c：真正执行命令并返回不同类型的结果
 *
 * 为什么这一章还不去实现更复杂的命令行解析？
 * - 因为 ch06 的重点不是“输入怎么写得更像 redis-cli”
 * - 而是“客户端怎样把不同结果类型看懂”
 */

#include "proto.h"
#include "codec.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * 把用户输入的一行文本命令，编码成请求 payload。
 *
 * 参数：
 * - line：原地可修改的输入行
 * - out_buf：返回新分配的请求缓冲区
 * - out_len：返回请求 payload 长度
 *
 * 返回值：
 * - 0：成功
 * - -1：失败
 *
 * 调用时机：
 * - main 循环中，读到一行终端输入之后
 *
 * 执行流程概览：
 * 1. 先按空格把一行文本切成参数
 * 2. 计算 payload 总长度
 * 3. 依次写入 argc、每个参数的长度和参数字节
 *
 * 易错点：
 * - 当前还是最简单的空格切分，不支持带空格参数
 * - out_buf 由这里分配，调用方用完后必须 free
 */
static int build_req(char *line,
                     uint8_t **out_buf,
                     uint32_t *out_len)
{
    char *argv[16];
    uint32_t lens[16];
    uint32_t argc = 0;

    /*
     * 当前阶段先用最简单的方式：按空格切分。
     *
     * 这样做的优点是：
     * - 写法简单，便于把注意力放在协议和数据库主线
     *
     * 局限也很明显：
     * - 还不适合处理包含空格的参数
     *
     * 这是当前阶段刻意保留的教学限制。
     */
    char *token = strtok(line, " ");
    while (token && argc < 16) {
        argv[argc] = token;
        lens[argc] = (uint32_t)strlen(token);
        argc++;
        token = strtok(NULL, " ");
    }

    if (argc == 0)
        return -1;

    uint32_t total = 4;
    for (uint32_t i = 0; i < argc; i++) {
        total += 4;
        total += lens[i];
    }

    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return -1;

    uint8_t *p = buf;
    uint32_t net_argc = htonl(argc);
    memcpy(p, &net_argc, 4);
    p += 4;

    for (uint32_t i = 0; i < argc; i++) {
        uint32_t net_len = htonl(lens[i]);
        memcpy(p, &net_len, 4);
        p += 4;
        memcpy(p, argv[i], lens[i]);
        p += lens[i];
    }

    *out_buf = buf;
    *out_len = total;
    return 0;
}

/*
 * 打印系统调用失败信息并退出。
 *
 * 参数：
 * - msg：出错阶段提示词
 *
 * 返回值：
 * - 无，函数内部直接 exit(1)
 */
static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

/*
 * 按响应类型打印服务器结果。
 *
 * 参数：
 * - resp：已经解析好的响应视图
 *
 * 返回值：
 * - 无
 *
 * 调用时机：
 * - 客户端收到完整响应并调用 parse_resp 成功之后
 *
 * 为什么这一章要单独提这个函数？
 * - 因为 ch06 的重点就是“不同响应类型怎样显示得更清楚”
 * - 把打印逻辑单独集中起来，更方便你对照每种类型的含义
 */
static void print_resp(const RespView *resp)
{
    switch (resp->type) {
    case RESP_OK:
        printf("< OK\n");
        break;

    case RESP_ERR:
        if (resp->len > 0 && resp->data) {
            printf("< (ERR) %.*s\n", (int)resp->len, resp->data);
        } else {
            printf("< (ERR)\n");
        }
        break;

    case RESP_STR:
        printf("< %.*s\n", (int)resp->len, resp->data);
        break;

    case RESP_INT:
        printf("< %lld\n", (long long)resp->integer);
        break;

    case RESP_NIL:
        printf("< (nil)\n");
        break;
    }
}

int main(void)
{
    /*
     * 连接到本机服务端。
     *
     * 这一章并没有改动连接逻辑，
     * 因为 ch06 的重点只在“结果类型解释”。
     */
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        die("socket");

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(15001);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        die("connect");

    char line[4096];
    while (1) {
        printf("> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin))
            break;

        size_t n = strlen(line);
        if (n && line[n - 1] == '\n') {
            line[n - 1] = 0;
            n--;
        }

        uint8_t *req = NULL;
        uint32_t req_len = 0;
        if (build_req(line, &req, &req_len) != 0)
            continue;

        /*
         * send_msg 负责自动在 payload 前补 4 字节长度前缀。
         */
        if (send_msg(fd, req, req_len) != 0)
            die("send_msg");
        free(req);

        uint8_t *resp_buf = NULL;
        uint32_t resp_len = 0;
        int r = recv_msg(fd, &resp_buf, &resp_len);
        if (r == 1) {
            printf("(server closed)\n");
            break;
        }
        if (r != 0)
            die("recv_msg");

        RespView resp;
        if (parse_resp(resp_buf, resp_len, &resp) != 0) {
            printf("(bad response)\n");
            free(resp_buf);
            continue;
        }

        print_resp(&resp);
        free(resp_buf);
    }

    close(fd);
    return 0;
}
