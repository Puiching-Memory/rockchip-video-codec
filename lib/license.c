/**
 * @file license.c
 * @brief 1机1码授权实现（Ed25519 非对称签名 + 硬件指纹）。
 *
 * 仅当 `RKVC_ENABLE_LICENSE=1` 时编译。
 *
 * 许可证二进制布局（104 字节，小端）：
 *   off 0   uint32 magic       = RKVC_LICENSE_MAGIC
 *   off 4   uint32 product_id
 *   off 8   uint8  machine_id[32]  SHA-256 指纹
 *   off 40  uint8  signature[64]   Ed25519 签名，覆盖 off 0..39
 *
 * 注册码 = base64(blob)。授权文件 = 注册码文本（可含换行/空白）。
 * 不含有效期字段——授权一经签发永久有效（1机1码）。
 */

#include "rkvc/license.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>
#include "license_layout.h"
#include "license_machine.h"
#include "license_b64.h"

/* 内嵌公钥（lib/license_pubkey.c，已 XOR 混淆，运行时再解密） */
extern const uint8_t rkvc_license_pubkey_enc[32];
extern const unsigned rkvc_license_pubkey_len;

/* 解密公钥：key_i = 0xA5 ^ (i * 7)，与 CMake 生成逻辑保持一致。 */
static void pubkey_decrypt(uint8_t *out, size_t len)
{
    for (size_t i = 0; i < len; i++)
        out[i] = (uint8_t)(rkvc_license_pubkey_enc[i] ^ (0xA5u ^ (unsigned char)(i * 7u)));
}

/* 字符串混淆：key 为基值，逐字节按 (key + i) 异或。 */
static void lic_deobf_str(const uint8_t *in, size_t len, char *out, uint8_t key)
{
    for (size_t i = 0; i < len; i++)
        out[i] = (char)(in[i] ^ (uint8_t)(key + i));
    out[len] = '\0';
}

#if defined(__linux__) && defined(__GNUC__)
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>

/* 轻量反调试：若当前进程已处于被跟踪状态，self-trace 会失败。注意：
 * PTRACE_TRACEME 成功后 TracerPid 会变为父进程 PID，因此只以 ptrace
 * 是否失败作为判断依据，避免误报正常的自跟踪。 */
static int lic_is_being_debugged(void)
{
    static int checked = 0;
    static int result = 0;
    if (checked) return result;
    checked = 1;

    if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0) {
        result = 1;
        return 1;
    }
    return 0;
}
#else
static int lic_is_being_debugged(void) { return 0; }
#endif

/* 公共常量（license.h）须与共享线格式常量（license_layout.h）一致 */
_Static_assert(RKVC_LICENSE_BLOB_SIZE ==
               RKVC_LICENSE_SIGNED_LEN + RKVC_LICENSE_SIG_LEN,
               "public blob size must equal signed + sig region");
_Static_assert(RKVC_MACHINE_ID_HEX_LEN == LIC_MACHINE_ID_HEX_LEN,
               "machine-id hex length mismatch between public/internal");

static uint32_t get_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ── 机器码（委托共享实现 license_machine.c，与 rkvc_lic 单一来源） ── */

rkvc_err rkvc_machine_id(char *out_hex, size_t out_size)
{
    if (!out_hex || out_size < RKVC_MACHINE_ID_HEX_LEN)
        return RKVC_ERR_INVALID;

    if (sodium_init() < 0)
        return RKVC_ERR_INTERNAL;

    if (lic_machine_id_hex(out_hex, out_size) != 0)
        return RKVC_ERR_HW;
    return RKVC_OK;
}

/* ── Ed25519 签名校验 ──────────────────────────────────────────── */

static int ed25519_verify(const uint8_t *data, size_t data_len,
                          const uint8_t *sig, size_t sig_len)
{
    if (rkvc_license_pubkey_len != crypto_sign_PUBLICKEYBYTES ||
        sig_len != crypto_sign_BYTES)
        return 0;

    uint8_t pubkey[32];
    pubkey_decrypt(pubkey, sizeof(pubkey));

    /* libsodium 原生支持 Ed25519 验签，无需 DER/PEM 包装 */
    return crypto_sign_verify_detached(sig, data, (unsigned long)data_len,
                                       pubkey) == 0 ? 1 : 0;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
    out[len * 2] = '\0';
}

/* ── 公共 API ───────────────────────────────────────────────────── */

rkvc_err rkvc_license_verify_blob(const uint8_t *blob, size_t len,
                                  rkvc_license_info *info)
{
    if (!blob)
        return RKVC_ERR_INVALID;
    if (len != RKVC_LICENSE_BLOB_SIZE)
        return RKVC_ERR_INVALID;

    if (sodium_init() < 0)
        return RKVC_ERR_INTERNAL;

    if (info)
        memset(info, 0, sizeof(*info));

    const uint8_t *signed_region = blob;
    const uint8_t *sig = blob + RKVC_LICENSE_SIGNED_LEN;

    uint32_t magic = get_u32_le(blob);
    uint32_t product_id = get_u32_le(blob + 4);
    const uint8_t *license_mid = blob + 8;

    /* 1) 签名校验 */
    int sig_ok = ed25519_verify(signed_region, RKVC_LICENSE_SIGNED_LEN,
                                sig, RKVC_LICENSE_SIG_LEN);
    if (info) {
        info->product_id = product_id;
        bytes_to_hex(license_mid, 32, info->machine_id);
        info->signature_valid = sig_ok;
    }
    if (!sig_ok)
        return RKVC_ERR_LICENSE;

    /* 2) magic / product */
    if (magic != RKVC_LICENSE_MAGIC || product_id != RKVC_PRODUCT_RKVC)
        return RKVC_ERR_LICENSE;

    /* 3) 机器码匹配 */
    /* 统一比较 lic_hex：info 是否为 NULL 均成立（info!=NULL 时与已填充的
       info->machine_id 字段等价）。 */
    char lic_hex[RKVC_MACHINE_ID_HEX_LEN];
    bytes_to_hex(license_mid, 32, lic_hex);

    char local_hex[RKVC_MACHINE_ID_HEX_LEN] = {0};
    rkvc_err e = rkvc_machine_id(local_hex, sizeof(local_hex));
    if (e != RKVC_OK) {
        if (info)
            memcpy(info->local_machine_id, local_hex, sizeof(local_hex));
        return e;
    }
    int match = (strcmp(local_hex, lic_hex) == 0);
    if (info) {
        memcpy(info->local_machine_id, local_hex, sizeof(local_hex));
        info->machine_matches = match;
    }
    if (!match)
        return RKVC_ERR_LICENSE;

    /* 4) 通过：授权一经签发永久有效，无有效期校验 */
    if (info)
        info->valid = 1;
    return RKVC_OK;
}

rkvc_err rkvc_license_default_path(char *out_path, size_t out_size)
{
    if (!out_path || out_size == 0)
        return RKVC_ERR_INVALID;

    const char *env = getenv("RKVC_LICENSE_FILE");
    if (env && env[0] != '\0') {
        if (strlen(env) >= out_size)
            return RKVC_ERR_INVALID;
        strcpy(out_path, env);
        return RKVC_OK;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0')
        return RKVC_ERR_NOT_FOUND;

    /* 默认路径后缀 "/.config/rkvc/license.lic"，XOR 混淆存储。 */
    static const uint8_t lic_path_suffix_obf[] = {
        0x94, 0x92, 0xde, 0xd1, 0xd1, 0xa6, 0xa8, 0xa5,
        0xec, 0xb6, 0xae, 0xb0, 0xa4, 0xe7, 0xa5, 0xa3,
        0xa8, 0xa9, 0xa3, 0xbd, 0xaa, 0xfe, 0xbd, 0xbb,
        0xb0
    };
    char suffix[32];
    lic_deobf_str(lic_path_suffix_obf, sizeof(lic_path_suffix_obf), suffix, 0xBBu);

    int n = snprintf(out_path, out_size, "%s%s", home, suffix);
    if (n < 0 || (size_t)n >= out_size)
        return RKVC_ERR_INVALID;
    return RKVC_OK;
}

rkvc_err rkvc_license_verify_file(const char *path, rkvc_license_info *info)
{
    if (!path)
        return RKVC_ERR_INVALID;

    FILE *f = fopen(path, "rb");
    if (!f)
        return RKVC_ERR_IO;

    /* 注册码文本最长 ~256 字符即可装下 104 字节 base64 */
    char text[512];
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';

    uint8_t blob[RKVC_LICENSE_BLOB_SIZE];
    size_t blob_len = 0;
    if (lic_b64_decode(text, n, blob, sizeof(blob), &blob_len) != 0 ||
        blob_len != RKVC_LICENSE_BLOB_SIZE)
        return RKVC_ERR_LICENSE;

    return rkvc_license_verify_blob(blob, blob_len, info);
}

rkvc_err rkvc_license_check(rkvc_license_info *info)
{
    if (lic_is_being_debugged())
        return RKVC_ERR_LICENSE;

    char path[512];
    rkvc_err e = rkvc_license_default_path(path, sizeof(path));
    if (e != RKVC_OK)
        return RKVC_ERR_UNLICENSED;
    if (access(path, R_OK) != 0)
        return RKVC_ERR_UNLICENSED;
    return rkvc_license_verify_file(path, info);
}
