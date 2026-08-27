/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file model_crypt.c
 * @brief 模型文件自研加密层：读取（透明解密）+ 每机 model.key 授权校验。
 *
 * 与 Rockchip rknn_crypt_tool（密钥内嵌公开 runtime，仅防小白拷贝）不同，
 * 本层密钥完全自主：
 *  1. 模型体 = XSalsa20-Poly1305(data_key)，文件头见 model_crypt_layout.h；
 *  2. data_key 不随包分发，而是密封在每机一份的 model.key 里
 *     （内嵌 master_key 加密，明文含目标机机器码）；
 *  3. 运行时先解 model.key、校验本机机器码（复用 1机1码 指纹），
 *     通过才解密模型体。未授权机器拿不到 data_key，模型不可解密。
 *
 * 对调用方透明：rkvc_model_crypt_load_file() 对未加密模型原样透传，
 * 加密模型返回解密后的明文缓冲。
 */
#include "internal.h"

#ifdef RKVC_ENABLE_MODEL_CRYPT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "license_machine.h"
#include "model_crypt_layout.h"

/** 由 lib/model_key.c（演示）或 CMake 生成文件（生产）提供 */
extern const uint8_t model_masterkey_obfuscated[32];

/** 模型文件大小上限（与 MLVC_MAX_MODEL_BYTES 对齐，防误读巨型文件） */
#define MODEL_CRYPT_MAX_BYTES ((size_t)256 * 1024 * 1024)

int rkvc_model_crypt_is_encrypted(const void *buf, size_t size)
{
    if (!buf || size < RKVC_MODEL_ENC_MAGIC_LEN)
        return 0;
    return memcmp(buf, RKVC_MODEL_ENC_MAGIC, RKVC_MODEL_ENC_MAGIC_LEN) == 0;
}

static void master_key_deobf(uint8_t out[32])
{
    /* 与 license pubkey 相同的混淆参数：key_i = 0xA5 ^ (i * 7)（截 8 位） */
    for (unsigned i = 0; i < 32; i++)
        out[i] = model_masterkey_obfuscated[i] ^ (uint8_t)(0xA5 ^ (i * 7));
}

static rkvc_err read_whole_file(const char *path, size_t max_bytes,
                                void **out_buf, size_t *out_size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return RKVC_ERR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    const long fsize = ftell(fp);
    if (fsize <= 0 || (size_t)fsize > max_bytes ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    void *buf = rkvc_malloc((size_t)fsize);
    if (!buf) {
        fclose(fp);
        return RKVC_ERR_NOMEM;
    }
    if (fread(buf, 1, (size_t)fsize, fp) != (size_t)fsize) {
        fclose(fp);
        rkvc_free(buf);
        return RKVC_ERR_IO;
    }
    fclose(fp);
    *out_buf = buf;
    *out_size = (size_t)fsize;
    return RKVC_OK;
}

static rkvc_err model_key_default_path(char *out_path, size_t out_size)
{
    const char *env = getenv("RKVC_MODEL_KEY_FILE");
    if (env && env[0] != '\0') {
        if (strlen(env) >= out_size)
            return RKVC_ERR_INVALID;
        strcpy(out_path, env);
        return RKVC_OK;
    }

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0')
        return RKVC_ERR_NOT_FOUND;

    /* 默认路径后缀 "/.config/rkvc/model.key"，XOR 混淆存储（key 0xBB）。 */
    static const uint8_t key_path_suffix_obf[] = {
        0x94, 0x95, 0xd8, 0xd4, 0xd5, 0xdd, 0xd2, 0xdc,
        0x94, 0xc9, 0xd0, 0xcd, 0xd8, 0x94, 0xd6, 0xd4,
        0xdf, 0xde, 0xd7, 0x95, 0xd0, 0xde, 0xc2
    };
    char suffix[32];
    for (size_t i = 0; i < sizeof(key_path_suffix_obf); i++)
        suffix[i] = (char)(key_path_suffix_obf[i] ^ 0xBB);
    suffix[sizeof(key_path_suffix_obf)] = '\0';

    const int n = snprintf(out_path, out_size, "%s%s", home, suffix);
    if (n < 0 || (size_t)n >= out_size)
        return RKVC_ERR_INVALID;
    return RKVC_OK;
}

/**
 * 读取并解密 model.key，校验机器码，取出 data_key。
 * @param data_key_out 32 字节数据密钥（调用方用后清零）。
 */
static rkvc_err model_key_load(uint8_t data_key_out[32])
{
    char path[512];
    if (model_key_default_path(path, sizeof(path)) != RKVC_OK)
        return RKVC_ERR_UNLICENSED;

    void *buf = NULL;
    size_t size = 0;
    if (read_whole_file(path, 4096, &buf, &size) != RKVC_OK) {
        RKVC_LOG("model.key not found: %s", path);
        return RKVC_ERR_UNLICENSED;
    }
    if (size != RKVC_MODEL_KEY_FILE_LEN ||
        memcmp(buf, RKVC_MODEL_KEY_MAGIC, RKVC_MODEL_KEY_MAGIC_LEN) != 0) {
        rkvc_free(buf);
        RKVC_LOG("model.key invalid format: %s", path);
        return RKVC_ERR_LICENSE;
    }

    const uint8_t *p = buf;
    const uint32_t version = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                             ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (version != RKVC_MODEL_KEY_VERSION) {
        rkvc_free(buf);
        RKVC_LOG("model.key unsupported version %u", version);
        return RKVC_ERR_LICENSE;
    }
    const uint8_t *nonce = p + 12;
    uint8_t plain[RKVC_MODEL_KEY_PLAIN_LEN];
    uint8_t master[32];
    master_key_deobf(master);
    const int rc = crypto_secretbox_open_easy(
        plain, p + 12 + RKVC_MODEL_ENC_NONCE_LEN,
        RKVC_MODEL_ENC_MAC_LEN + RKVC_MODEL_KEY_PLAIN_LEN, nonce, master);
    sodium_memzero(master, sizeof(master));
    rkvc_free(buf);
    if (rc != 0) {
        RKVC_LOG("model.key MAC verification failed");
        return RKVC_ERR_LICENSE;
    }

    /* 机器码校验：与 1机1码 同一指纹算法 */
    lic_fp_info fp_info;
    if (lic_machine_id_collect(&fp_info) != 0) {
        sodium_memzero(plain, sizeof(plain));
        RKVC_LOG("model.key: machine id unavailable (dt=%s otp=%s mac=%s)",
                 fp_info.note_dt, fp_info.note_otp, fp_info.note_mac);
        return RKVC_ERR_LICENSE;
    }
    if (memcmp(plain + 32, fp_info.machine_id,
               RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN) != 0) {
        sodium_memzero(plain, sizeof(plain));
        RKVC_LOG("model.key machine id mismatch (local source=%s path=%s)",
                 fp_info.tag, fp_info.path);
        return RKVC_ERR_LICENSE;
    }

    memcpy(data_key_out, plain, 32);
    sodium_memzero(plain, sizeof(plain));
    return RKVC_OK;
}

rkvc_err rkvc_model_crypt_load_file(const char *path, void **out_buf,
                                    size_t *out_size)
{
    if (!path || !out_buf || !out_size)
        return RKVC_ERR_INVALID;

    if (sodium_init() < 0) {
        RKVC_LOG("libsodium initialization failed");
        return RKVC_ERR_INTERNAL;
    }

    void *buf = NULL;
    size_t size = 0;
    const rkvc_err rerr = read_whole_file(path, MODEL_CRYPT_MAX_BYTES,
                                          &buf, &size);
    if (rerr != RKVC_OK)
        return rerr;

    if (!rkvc_model_crypt_is_encrypted(buf, size)) {
        /* 明文模型：原样透传 */
        *out_buf = buf;
        *out_size = size;
        return RKVC_OK;
    }

    if (size < RKVC_MODEL_ENC_HDR_LEN) {
        rkvc_free(buf);
        RKVC_LOG("encrypted model %s: incomplete header (%zu bytes)", path, size);
        return RKVC_ERR_FORMAT;
    }

    const uint8_t *hdr = buf;
    const uint32_t version = (uint32_t)hdr[8] | ((uint32_t)hdr[9] << 8) |
                             ((uint32_t)hdr[10] << 16) | ((uint32_t)hdr[11] << 24);
    const uint32_t flags = (uint32_t)hdr[12] | ((uint32_t)hdr[13] << 8) |
                           ((uint32_t)hdr[14] << 16) | ((uint32_t)hdr[15] << 24);
    uint64_t plain_len = 0;
    for (int i = 7; i >= 0; i--)
        plain_len = (plain_len << 8) | hdr[16 + i];
    if (version != RKVC_MODEL_ENC_VERSION || flags != 0 ||
        plain_len == 0 || plain_len > MODEL_CRYPT_MAX_BYTES) {
        rkvc_free(buf);
        RKVC_LOG("encrypted model %s: unsupported header (v%u flags 0x%x)",
                 path, version, flags);
        return RKVC_ERR_FORMAT;
    }
    const size_t expected = RKVC_MODEL_ENC_HDR_LEN +
                            RKVC_MODEL_ENC_MAC_LEN + (size_t)plain_len;
    if (size != expected) {
        rkvc_free(buf);
        RKVC_LOG("encrypted model %s: truncated (got %zu want %zu)",
                 path, size, expected);
        return RKVC_ERR_FORMAT;
    }

    uint8_t data_key[32];
    const rkvc_err kerr = model_key_load(data_key);
    if (kerr != RKVC_OK) {
        rkvc_free(buf);
        return kerr;
    }

    /* 原地解密：密文区 [hdr+48, hdr+48+MAC+plain) → 明文 plain 字节。
     * 解密成功后把缓冲区缩成纯明文交给调用方。 */
    uint8_t *cipher = (uint8_t *)buf + RKVC_MODEL_ENC_HDR_LEN;
    if (crypto_secretbox_open_easy(cipher, cipher,
                                   RKVC_MODEL_ENC_MAC_LEN + (size_t)plain_len,
                                   hdr + RKVC_MODEL_ENC_HDR_LEN -
                                   RKVC_MODEL_ENC_NONCE_LEN, data_key) != 0) {
        sodium_memzero(data_key, sizeof(data_key));
        rkvc_secure_zero_free(buf, size);
        RKVC_LOG("encrypted model %s: decryption failed", path);
        return RKVC_ERR_LICENSE;
    }
    sodium_memzero(data_key, sizeof(data_key));

    /* 前移明文覆盖头部，返回干净的明文缓冲 */
    memmove(buf, cipher, (size_t)plain_len);
    memset((uint8_t *)buf + plain_len, 0, size - (size_t)plain_len);
    RKVC_LOG("model %s: decrypted %llu bytes (model_crypt v%u)",
             path, (unsigned long long)plain_len, version);
    *out_buf = buf;
    *out_size = (size_t)plain_len;
    return RKVC_OK;
}

#endif /* RKVC_ENABLE_MODEL_CRYPT */
