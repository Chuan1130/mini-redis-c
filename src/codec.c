/*
 * 本文件属于哪一章：
 * - ch06：整理响应语义，让命令结果表达更清晰
 *
 * 本文件负责什么：
 * - 解析 request payload
 * - 组装 response payload
 * - 解析 response payload
 * - 让协议层能够明确区分 OK / ERR / STR / INT / NIL
 *
 * 本文件和其他文件如何配合：
 * - include/codec.h：声明本文件实现的结构和接口
 * - src/tcp_server.c：命令执行完后构造 Resp，再调用 make_resp
 * - src/tcp_client.c：收到响应后调用 parse_resp，再按类型打印
 * - src/proto.c：负责外层长度前缀，本文件只关心 payload 内部布局
 *
 * 这一章最重要的学习点：
 * - 网络层只负责“包有没有收完整”
 * - 但真正决定“这条响应是什么意思”的，是协议层
 * - 当命令变多后，协议层如果没有明确类型，客户端就会越来越难判断结果
 */

#include "codec.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/*
 * 从当前游标处读取一个 4 字节无符号整数。
 *
 * 参数：
 * - cur：当前读取游标，会在成功后向前推进
 * - end：缓冲区末尾后一位，用来做边界检查
 * - out：用于带出读取结果
 *
 * 返回值：
 * - true：读取成功
 * - false：剩余字节不足 4，读取失败
 *
 * 调用时机：
 * - 解析请求参数个数
 * - 解析参数长度
 * - 解析响应类型
 * - 解析字符串响应长度
 *
 * 易错点：
 * - 这里读取的是网络字节序，所以必须 ntohl
 * - 不能直接把 cur 当成 uint32_t * 解引用，否则可能有对齐问题
 */
bool read_u32(const uint8_t **cur,
              const uint8_t *end,
              uint32_t *out)
{
    if (*cur + 4 > end)
        return false;

    uint32_t net = 0;
    memcpy(&net, *cur, 4);
    *cur += 4;

    *out = ntohl(net);
    return true;
}

/*
 * 从当前游标处读取一个 8 字节有符号整数。
 *
 * 参数：
 * - cur：当前读取游标，会在成功后向前推进
 * - end：缓冲区末尾后一位，用来做边界检查
 * - out：用于带出读取结果
 *
 * 返回值：
 * - true：读取成功
 * - false：剩余字节不足 8，读取失败
 *
 * 为什么这一章要单独支持 8 字节整数？
 * - 因为 EXISTS、DEL 这类命令更适合返回整数结果
 * - 如果后面还想补 TTL 一类命令，整数返回也会很自然
 *
 * 这里采用手工拼装的方式，而不是依赖平台现成的 htonll/ntohll，
 * 是为了兼容性更稳定，也更适合教学展示“网络字节序到底在做什么”。
 */
static bool read_i64(const uint8_t **cur,
                     const uint8_t *end,
                     int64_t *out)
{
    if (*cur + 8 > end)
        return false;

    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | (*cur)[i];
    }

    *cur += 8;
    *out = (int64_t)v;
    return true;
}

/*
 * 把一个 8 字节有符号整数按大端序写入目标缓冲区。
 *
 * 参数：
 * - dst：目标写入位置，必须至少有 8 字节空间
 * - value：要写入的整数
 *
 * 返回值：
 * - 无
 *
 * 执行流程概览：
 * - 从高字节到低字节依次拆出 value 的每 8 位
 * - 再按网络传输习惯写成大端序
 */
static void write_i64(uint8_t *dst, int64_t value)
{
    uint64_t v = (uint64_t)value;
    for (int i = 7; i >= 0; i--) {
        dst[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

/*
 * 从当前游标处读取 n 个原始字节。
 *
 * 参数：
 * - cur：当前读取游标，会在成功后向前推进
 * - end：缓冲区末尾后一位，用来做边界检查
 * - n：要读取的字节数
 * - out：用于带出这段字节的起始地址
 *
 * 返回值：
 * - true：读取成功
 * - false：剩余空间不足
 *
 * 注意：
 * - 这里不会复制数据
 * - out 只是指向原始缓冲区中的一段视图
 */
bool read_bytes(const uint8_t **cur,
                const uint8_t *end,
                uint32_t n,
                const uint8_t **out)
{
    if (*cur + n > end)
        return false;

    *out = *cur;
    *cur += n;
    return true;
}

/*
 * 解析 request payload。
 *
 * 参数：
 * - data/size：完整请求体，不包含最外层长度前缀
 * - out：用于带出解析结果
 *
 * 返回值：
 * - 0：解析成功
 * - -1：解析失败
 *
 * 调用时机：
 * - 服务端已经完整收下一条请求之后
 *
 * 执行流程概览：
 * 1. 先读参数个数 argc
 * 2. 再依次读取每个参数的长度和内容
 * 3. 最后检查有没有正好读完
 */
int parse_req(const uint8_t *data,
              size_t size,
              Args *out)
{
    const uint8_t *cur = data;
    const uint8_t *end = data + size;
    uint32_t nstr = 0;

    if (!read_u32(&cur, end, &nstr))
        return -1;

    if (nstr == 0 || nstr > MAX_ARGS)
        return -1;

    out->argc = nstr;

    for (uint32_t i = 0; i < nstr; i++) {
        uint32_t len = 0;
        if (!read_u32(&cur, end, &len))
            return -1;

        const uint8_t *p = NULL;
        if (!read_bytes(&cur, end, len, &p))
            return -1;

        out->argv[i] = p;
        out->lens[i] = len;
    }

    if (cur != end)
        return -1;

    return 0;
}

/*
 * 解析 response payload。
 *
 * 参数：
 * - data/size：完整响应体，不包含最外层长度前缀
 * - out：用于带出解析后的响应视图
 *
 * 返回值：
 * - 0：解析成功
 * - -1：解析失败
 *
 * 调用时机：
 * - 客户端通过 recv_msg 收到一条完整响应之后
 *
 * 执行流程概览：
 * 1. 先读出 type
 * 2. 再根据 type 决定后面需要读什么
 * 3. 最后检查是否正好读完
 *
 * 易错点：
 * - 不同类型的响应，后续字段长度并不一样
 * - 所以一定不能“想当然”地把所有响应都当成同一种格式解析
 */
int parse_resp(const uint8_t *data,
               size_t size,
               RespView *out)
{
    const uint8_t *cur = data;
    const uint8_t *end = data + size;
    uint32_t type_u32 = 0;

    if (!read_u32(&cur, end, &type_u32))
        return -1;

    out->type = (RespType)type_u32;
    out->data = NULL;
    out->len = 0;
    out->integer = 0;

    switch (out->type) {
    case RESP_OK:
    case RESP_NIL:
        break;

    case RESP_ERR:
    case RESP_STR: {
        uint32_t len = 0;
        const uint8_t *p = NULL;
        if (!read_u32(&cur, end, &len))
            return -1;
        if (!read_bytes(&cur, end, len, &p))
            return -1;
        out->data = p;
        out->len = len;
        break;
    }

    case RESP_INT:
        if (!read_i64(&cur, end, &out->integer))
            return -1;
        break;

    default:
        return -1;
    }

    if (cur != end)
        return -1;

    return 0;
}

/*
 * 根据响应类型，计算响应体需要多少字节。
 *
 * 参数：
 * - resp：命令层构造好的响应语义对象
 *
 * 返回值：
 * - 成功：返回响应体长度（不含最外层 4 字节长度前缀）
 * - 失败：返回 0，表示响应对象本身不合法
 *
 * 为什么单独提这个函数？
 * - 因为 make_resp 需要先知道最终长度，才能一次性分配缓冲区
 */
static uint32_t resp_body_len(const Resp *resp)
{
    switch (resp->type) {
    case RESP_OK:
    case RESP_NIL:
        return 4;

    case RESP_ERR:
    case RESP_STR:
        return 4 + 4 + resp->len;

    case RESP_INT:
        return 4 + 8;
    }

    return 0;
}

/*
 * 组装响应。
 *
 * 参数：
 * - resp：命令层构造好的响应语义对象
 * - out_len：返回完整可发送缓冲区长度
 *
 * 返回值：
 * - 成功：返回新分配的完整可发送缓冲区
 * - 失败：返回 NULL
 *
 * 调用时机：
 * - 服务端命令执行结束后，准备把结果发回客户端
 *
 * 执行流程概览：
 * 1. 先根据响应类型算出 body 长度
 * 2. 在最前面写最外层长度前缀
 * 3. 再写响应 type
 * 4. 最后根据不同 type，写不同字段
 */
uint8_t *make_resp(const Resp *resp, uint32_t *out_len)
{
    uint32_t body_len = resp_body_len(resp);
    if (body_len == 0)
        return NULL;

    uint32_t total = 4 + body_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf)
        return NULL;

    uint32_t net_body_len = htonl(body_len);
    memcpy(buf, &net_body_len, 4);

    uint8_t *p = buf + 4;
    uint32_t net_type = htonl((uint32_t)resp->type);
    memcpy(p, &net_type, 4);
    p += 4;

    switch (resp->type) {
    case RESP_OK:
    case RESP_NIL:
        break;

    case RESP_ERR:
    case RESP_STR: {
        uint32_t net_len = htonl(resp->len);
        memcpy(p, &net_len, 4);
        p += 4;
        if (resp->len > 0 && resp->data)
            memcpy(p, resp->data, resp->len);
        break;
    }

    case RESP_INT:
        write_i64(p, resp->integer);
        break;
    }

    *out_len = total;
    return buf;
}
