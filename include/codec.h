#pragma once

/*
 * 本文件属于哪一章：
 * - ch06：整理响应语义，让命令结果表达更清晰
 *
 * 本文件负责什么：
 * - 定义 request payload 的解析接口
 * - 定义 response payload 的类型系统和编解码接口
 * - 让“命令执行结果”在协议层有更清楚的表达方式
 *
 * 可以把当前协议分成两层来理解：
 * 1. proto.* 负责最外层“消息边界”
 * 2. codec.* 负责消息内部“字段怎么摆放”
 *
 * 本文件和其他文件如何配合：
 * - src/codec.c：真正实现这里声明的读写逻辑
 * - src/tcp_server.c：命令执行完后，构造 Resp 再交给 make_resp
 * - src/tcp_client.c：收到响应后，调用 parse_resp 理解服务器返回的结果类型
 *
 * 当前请求 payload 格式：
 * [4字节 argc]
 * [4字节 arg1_len][arg1_bytes]
 * [4字节 arg2_len][arg2_bytes]
 * ...
 *
 * ch06 之后的响应 payload 格式不再只是“status + 原始字节”，
 * 而是先明确“这是什么类型的结果”，再决定后面怎么解释数据。
 *
 * 当前响应 payload 格式：
 *
 * 1. 所有响应都先以 4 字节 type 开头：
 *    [4字节 type]
 *
 * 2. 不同 type 的后续内容不同：
 *    - RESP_OK：
 *      [4字节 type]
 *    - RESP_NIL：
 *      [4字节 type]
 *    - RESP_ERR / RESP_STR：
 *      [4字节 type][4字节 len][len 字节数据]
 *    - RESP_INT：
 *      [4字节 type][8字节有符号整数]
 *
 * 为什么这一章要引入 RespType？
 * - 因为当前命令已经开始出现不同语义：
 *   - SET 更像“简单成功”
 *   - GET 更像“字符串结果”或“空结果”
 *   - DEL / EXISTS 更像“整数结果”
 * - 如果继续把它们都塞进“status + 可选数据”，客户端会越来越难分辨
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define MAX_ARGS 16 /* 当前请求最多支持 16 个参数 */

/*
 * Args 表示“已经解析好的请求参数视图”。
 *
 * 注意：
 * 这里的 argv[i] 只是指向原始请求缓冲区内部的某一段，
 * 并不会复制出新的字符串。
 * 所以原始请求缓冲区在使用 Args 期间必须保持有效。
 */
typedef struct {
    uint32_t argc;
    const uint8_t *argv[MAX_ARGS];
    uint32_t lens[MAX_ARGS];
} Args;

/*
 * RespType 表示“服务端这次到底返回了什么类型的结果”。
 *
 * 各个类型的含义：
 * - RESP_OK：
 *   代表简单成功，没有额外数据，常见于 SET
 * - RESP_ERR：
 *   代表命令执行失败，并且可能附带一段错误文本
 * - RESP_STR：
 *   代表一段普通字节串结果，当前主要用于 GET 成功
 * - RESP_INT：
 *   代表一个整数结果，当前主要用于 DEL / EXISTS
 * - RESP_NIL：
 *   代表“空结果 / 不存在”，当前主要用于 GET 不存在
 *
 * 为什么要单独保留 RESP_NIL，而不是直接把它当 ERR？
 * - 因为“key 不存在”很多时候不是执行失败
 * - 例如 GET missing，它更像“查询结果为空”
 * - 这和“命令拼错了”或“参数数量不对”不是一回事
 */
typedef enum {
    RESP_OK = 0,
    RESP_ERR = 1,
    RESP_STR = 2,
    RESP_INT = 3,
    RESP_NIL = 4
} RespType;

/*
 * Resp 表示服务端在命令执行阶段构造出来的“响应语义对象”。
 *
 * 字段说明：
 * - type：
 *   这次结果属于哪一种响应类型
 * - data / len：
 *   当 type 是 RESP_ERR 或 RESP_STR 时，表示附带的字节串内容
 * - integer：
 *   当 type 是 RESP_INT 时，表示要返回的整数值
 *
 * 为什么同时保留 data 和 integer？
 * - 因为字符串和整数在协议里需要用不同方式解释
 * - 如果只用一个 void * 或一段原始字节，会让调用方更难看懂
 *
 * 对初学者最重要的理解点：
 * - Resp 不是“已经编码好的网络字节”
 * - 它更像“命令执行结果的语义描述”
 * - 真正变成网络字节，是 make_resp 做的事
 */
typedef struct {
    RespType type;
    const uint8_t *data;
    uint32_t len;
    int64_t integer;
} Resp;

/*
 * RespView 表示“客户端已经解析好的响应视图”。
 *
 * 它和 Resp 很像，但用途不同：
 * - Resp：服务端内部用，表示“我想返回什么”
 * - RespView：客户端解析后用，表示“服务器实际发回了什么”
 *
 * 当前实现里为了教学清晰，两者字段保持一致。
 */
typedef struct {
    RespType type;
    const uint8_t *data;
    uint32_t len;
    int64_t integer;
} RespView;

/* 从当前游标位置读取一个网络字节序 u32。 */
bool read_u32(const uint8_t **cur,
              const uint8_t *end,
              uint32_t *out);

/* 从当前游标位置读取 n 个字节，并把起始地址返回给 out。 */
bool read_bytes(const uint8_t **cur,
                const uint8_t *end,
                uint32_t n,
                const uint8_t **out);

/*
 * 解析 request payload。
 *
 * 输入：
 * - data/size：完整请求体，不包含最外层长度前缀
 *
 * 输出：
 * - 成功返回 0，并把解析结果写入 out
 * - 失败返回 -1
 */
int parse_req(const uint8_t *data,
              size_t size,
              Args *out);

/*
 * 解析 response payload。
 *
 * 参数：
 * - data/size：完整响应体，不包含最外层长度前缀
 * - out：用于带出解析后的响应视图
 *
 * 返回值：
 * - 成功返回 0
 * - 失败返回 -1
 *
 * 调用时机：
 * - 客户端已经通过 recv_msg 收到一整条响应后
 *
 * 执行流程概览：
 * 1. 先读出响应类型 type
 * 2. 根据 type 决定后续字段怎么解释
 * 3. 把解析结果填入 RespView
 */
int parse_resp(const uint8_t *data,
               size_t size,
               RespView *out);

/*
 * 组装响应。
 *
 * 注意：
 * 这个函数返回的是“可以直接发送到 socket 的完整缓冲区”，
 * 也就是已经把最外层长度前缀一起写进去了。
 * 调用者使用完成后需要 free。
 *
 * 参数：
 * - resp：命令层构造好的响应语义对象
 * - out_len：返回完整可发送缓冲区长度
 *
 * 返回值：
 * - 成功：返回新分配的完整可发送缓冲区
 * - 失败：返回 NULL
 */
uint8_t *make_resp(const Resp *resp, uint32_t *out_len);
