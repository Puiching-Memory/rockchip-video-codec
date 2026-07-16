/**
 * @file license_b64.c
 * @brief 授权注册码 base64 解码（lib/license.c 与 tools/rkvc_lic.c 共享实现）。
 *
 * 解码逻辑的唯一定义，确保签发端 / 校验端字节级一致，并做严格 padding 校验。
 * 授权 blob 由 tools/rkvc_lic 的 b64_encode 产出，其末组残留位恒为零，
 * 故严格校验不会拒收合法授权。
 */
#include "license_b64.h"

static int b64_val(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

int lic_b64_decode(const char *src, size_t src_len,
                   uint8_t *dst, size_t dst_cap, size_t *out_len)
{
    size_t oi = 0;
    uint32_t acc = 0;
    int bits = 0;        /* acc 中未刷新的低位比特数 */
    int pad = 0;         /* 已见到的 '=' 个数 */
    int seen_pad = 0;

    for (size_t i = 0; i < src_len; i++) {
        unsigned char c = (unsigned char)src[i];

        /* 容忍任意位置的空白（换行/制表/空格） */
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t')
            continue;

        if (c == '=') {
            seen_pad = 1;
            if (++pad > 2)
                return -1;               /* padding 最多 2 个 */
            /* 末组须已有足够数据字符：3 字符配 1 '='（bits==2），
               2 字符配 2 '='（bits==4）。第 2 个 '=' 时 bits 不变，靠 pad 放行。 */
            if (pad == 1 && bits != 2 && bits != 4)
                return -1;
            continue;
        }

        /* 见到 padding 后再出现数据字符 → 非法 */
        if (seen_pad)
            return -1;

        int v = b64_val(c);
        if (v < 0)
            return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= dst_cap)
                return -1;
            dst[oi++] = (uint8_t)((acc >> bits) & 0xFF);
        }
    }

    /* 末组残位：0=完整组 / 2=配 1 padding / 4=配 2 padding，其它非法 */
    if (bits != 0 && bits != 2 && bits != 4)
        return -1;
    if (bits == 2 && pad != 1)
        return -1;
    if (bits == 4 && pad != 2)
        return -1;
    /* 末组残留位须为零（规范编码） */
    if (bits != 0 && (acc & ((1u << bits) - 1u)) != 0)
        return -1;

    *out_len = oi;
    return 0;
}
