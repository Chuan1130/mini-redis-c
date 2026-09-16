/*
 * 本文件作用：
 * 实现最外层消息收发逻辑。
 *
 * 这里解决的是 TCP 字节流中的“消息边界”问题。
 * 因为 TCP 只保证字节顺序，不保证你一次 read 就刚好拿到一整条请求。
 * 所以我们约定每条消息前面先带一个 4 字节长度。
 */

#include "proto.h"
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

int read_full(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t*)buf;
    size_t off = 0;

    /* 不断读取，直到读满 n 个字节为止。 */
    while (off < n) {
        ssize_t r = read(fd, p + off, n - off);
        if (r == 0) return 1;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

int write_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t off = 0;

    /* 不断写出，直到缓冲区里的数据全部发送完成。 */
    while (off < n) {
        ssize_t w = write(fd, p + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

int send_msg(int fd, const uint8_t *data, uint32_t len) {
    if (len > MAX_MSG_SIZE) return -1;

    /* 先发长度，再发 payload。客户端和服务端都遵守这个约定。 */
    uint32_t netlen = htonl(len);
    if (write_all(fd, &netlen, 4) != 0) return -1;
    if (len && write_all(fd, data, len) != 0) return -1;
    return 0;
}

int recv_msg(int fd, uint8_t **out, uint32_t *out_len) {
    *out = NULL;
    *out_len = 0;

    /* 第一步：先拿到后续 payload 的长度。 */
    uint32_t netlen = 0;
    int r = read_full(fd, &netlen, 4);
    if (r != 0) return r;

    uint32_t len = ntohl(netlen);
    if (len > MAX_MSG_SIZE) return -1;

    /* 第二步：按长度分配缓冲区，再把完整 payload 读进来。 */
    uint8_t *buf = (uint8_t*)malloc(len + 1);
    if (!buf) return -1;

    if (len) {
        r = read_full(fd, buf, len);
        if (r != 0) { free(buf); return r; }
    }
    /*
     * 末尾补一个 0，方便某些调试场景把内容当字符串看。
     * 这里真正有效的数据长度仍然是 len，不能把这个 0 算进去。
     */
    buf[len] = 0;
    *out = buf;
    *out_len = len;
    return 0;
}
