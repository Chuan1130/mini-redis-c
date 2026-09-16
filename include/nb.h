#pragma once

/*
 * 本文件作用：
 * 定义把文件描述符设置为非阻塞模式的接口。
 *
 * 非阻塞模式是当前服务端事件循环的基础：
 * read/write/accept 在暂时不能继续时，不会把整个进程卡住。
 */

/* 成功返回 0；失败返回 -1。 */
int set_nonblocking(int fd);
