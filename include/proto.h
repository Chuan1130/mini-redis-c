#pragma once

/*
 * 本文件作用：
 * 定义“最外层消息收发”接口。
 *
 * 这一层只关心一件事：
 * 如何在 TCP 字节流上，可靠地收发“一整条消息”。
 *
 * 当前项目采用的外层格式是：
 * [4字节长度][payload]
 *
 * 注意：
 * 这里不关心 payload 里面到底是 SET/GET 还是别的命令，
 * payload 的内部结构由 codec.* 负责。
 */

#include <stdint.h>
#include <stddef.h>

#define MAX_MSG_SIZE (1024 * 1024) /* 允许的最大消息体大小：1MB */

/*
 * 连续读取 n 个字节。
 *
 * 输入：
 * - fd：已经连接好的 socket
 * - buf：写入目标缓冲区
 * - n：必须读满的字节数
 *
 * 输出：
 * - 成功读满 -> 0
 * - 对端关闭 -> 1
 * - 发生错误 -> -1
 */
int read_full(int fd, void *buf, size_t n);

/*
 * 连续写出 n 个字节，直到全部发完。
 *
 * 输入：
 * - fd：socket
 * - buf：要发送的数据
 * - n：要发送的字节数
 *
 * 输出：
 * - 成功写完 -> 0
 * - 发生错误 -> -1
 */
int write_all(int fd, const void *buf, size_t n);

/*
 * 发送一条完整消息。
 *
 * 它会先发送 4 字节长度，再发送 payload 本体。
 */
int send_msg(int fd, const uint8_t *data, uint32_t len);

/*
 * 接收一条完整消息。
 *
 * 它会先读 4 字节长度，再按长度读取 payload。
 * 读取成功后会在堆上分配一块缓冲区，并把地址通过 out 返回。
 * 调用方使用完成后必须 free。
 *
 * 输出：
 * - 成功 -> 0
 * - 对端关闭 -> 1
 * - 发生错误 -> -1
 */
int recv_msg(int fd, uint8_t **out, uint32_t *out_len);
