/**
 * @file license_b64.h
 * @brief 授权注册码 base64 解码（lib/license.c 与 tools/rkvc_lic.c 共享）。
 *
 * 签发端写出的 base64 与校验端读入的 base64 必须行为一致，故解码逻辑单一来源。
 * 共享解码做严格校验（padding 位置/数量、末组残留位为零），畸形输入直接拒绝；
 * 实际授权校验仍以 Ed25519 签名为最终防线，此处为规范性收紧。
 *
 * 仅标准字母表（+ 起始的授权文件）使用；解码同时容忍 URL-safe（-_）。
 */
#ifndef RKVC_LICENSE_B64_H
#define RKVC_LICENSE_B64_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief 严格解码 base64（兼容标准 + URL-safe 字母表，容忍任意位置空白）。
 *
 * 校验规则：'=' 仅允许出现在末组且数量为 1 或 2；见到 '=' 后不得再出现数据字符；
 * 末组残留位必须为零（规范编码）。任一不满足返回 -1。
 *
 * @param src      输入文本。
 * @param src_len  输入长度。
 * @param dst      输出缓冲。
 * @param dst_cap  dst 容量；解码字节数超出则返回 -1。
 * @param out_len  回填解码字节数。
 * @return 0 成功；-1 输入非法或缓冲过小。
 */
int lic_b64_decode(const char *src, size_t src_len,
                   uint8_t *dst, size_t dst_cap, size_t *out_len);

#endif /* RKVC_LICENSE_B64_H */
