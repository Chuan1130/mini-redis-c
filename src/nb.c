/*
 * 本文件作用：
 * 把文件描述符切换到非阻塞模式。
 *
 * 当前服务端使用 poll 事件循环。
 * 如果 socket 还是阻塞模式，那么一次 read/write/accept 卡住后，
 * 整个事件循环都会被拖住。
 */

#include "nb.h"
#include <fcntl.h>
#include <unistd.h>

int set_nonblocking(int fd) {
    /* 先取出原来的标志位，再把 O_NONBLOCK 打开。 */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
