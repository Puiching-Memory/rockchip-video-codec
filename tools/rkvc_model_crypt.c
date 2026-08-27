/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/*
 * rkvc_model_crypt - 模型自研加密层辅助工具（打包方内部使用，不随包分发）。
 *
 * 密钥体系（见 lib/model_crypt_layout.h 与 lib/model_crypt.c）：
 *   master.key  32B 根密钥，构建时经 RKVC_MODEL_MASTERKEY_FILE 混淆内嵌进
 *               librkvc，仅用于密封 model.key；
 *   data.key    32B 数据密钥，用于加密模型文件本体，经 model.key 每机签发。
 *
 * 用法:
 *   rkvc_model_crypt genkey    -o <dir>               生成 master.key + data.key
 *   rkvc_model_crypt encrypt   -d data.key -i <in> [-o <out>]
 *                                                     加密模型（缺省原地替换）
 *   rkvc_model_crypt decrypt   -d data.key -i <in> -o <out>
 *                                                     解密（自检/排障）
 *   rkvc_model_crypt issue     -d data.key -m master.key -M <machine_id_hex>
 *                              -o model.key           为指定机器签发授权
 *   rkvc_model_crypt verify-key -m master.key -f model.key
 *                                                     解出并打印绑定机器码
 *   rkvc_model_crypt machine-id                     打印本机机器码（与 1机1码 同一指纹）
 */
#define _GNU_SOURCE 1

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sodium.h>

#include "license_machine.h"
#include "model_crypt_layout.h"

#define KEY_FILE_LEN 32u
#define MAX_MODEL_BYTES ((size_t)256 * 1024 * 1024)

/* RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN 是**字符数**（不含 NUL），而
 * lic_machine_id_hex() 的输出容量按 LIC_MACHINE_ID_HEX_LEN（含 NUL）校验；
 * 混用会让 machine-id 恒报“无可用硬件指纹”。两者关系由下面的断言钉住。 */
_Static_assert(RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN + 1u == LIC_MACHINE_ID_HEX_LEN,
               "machine id hex length drift: layout constant must be 64 chars "
               "(no NUL), one less than LIC_MACHINE_ID_HEX_LEN");

static int read_key_file(const char *path, uint8_t out[KEY_FILE_LEN])
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: 无法打开 %s: %s\n", path, strerror(errno));
        return -1;
    }
    uint8_t buf[KEY_FILE_LEN];
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    if (n != KEY_FILE_LEN) {
        fprintf(stderr, "error: %s 不是 %u 字节原始密钥（得到 %zu）\n",
                path, KEY_FILE_LEN, n);
        return -1;
    }
    memcpy(out, buf, KEY_FILE_LEN);
    return 0;
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "error: 无法写入 %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fwrite(data, 1, len, f) != len || fclose(f) != 0) {
        fprintf(stderr, "error: 写入 %s 失败\n", path);
        return -1;
    }
    return 0;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

/* ── genkey ──────────────────────────────────────────────────────── */

static int cmd_genkey(const char *dir)
{
    if (!dir) {
        fprintf(stderr, "用法: rkvc_model_crypt genkey -o <dir>\n");
        return 2;
    }
    char path[1024];
    const char *names[2] = { "master.key", "data.key" };
    for (int i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
        FILE *f = fopen(path, "rb");
        if (f) {
            fclose(f);
            fprintf(stderr, "error: %s 已存在，拒绝覆盖（删除后重试）\n", path);
            return 1;
        }
        uint8_t key[KEY_FILE_LEN];
        randombytes_buf(key, sizeof(key));
        if (write_file(path, key, sizeof(key)) != 0)
            return 1;
        printf("已生成: %s (32B, 切勿提交/随包分发)\n", path);
    }
    printf("提示: master.key 构建时经 -DRKVC_MODEL_MASTERKEY_FILE 内嵌进 librkvc\n");
    return 0;
}

/* ── encrypt / decrypt ───────────────────────────────────────────── */

static int read_model(const char *path, uint8_t **out, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: 无法打开 %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    const long sz = ftell(f);
    if (sz <= 0 || (size_t)sz > MAX_MODEL_BYTES || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "error: %s 大小无效（%ld）\n", path, sz);
        return -1;
    }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(buf);
        return -1;
    }
    fclose(f);
    *out = buf;
    *out_len = (size_t)sz;
    return 0;
}

static int cmd_encrypt(const char *data_key_path, const char *in_path,
                       const char *out_path)
{
    if (!data_key_path || !in_path) {
        fprintf(stderr, "用法: rkvc_model_crypt encrypt -d data.key -i <in> [-o <out>]\n");
        return 2;
    }
    uint8_t data_key[KEY_FILE_LEN];
    if (read_key_file(data_key_path, data_key) != 0)
        return 1;

    uint8_t *plain = NULL;
    size_t plain_len = 0;
    if (read_model(in_path, &plain, &plain_len) != 0)
        return 1;
    if (plain_len >= RKVC_MODEL_ENC_MAGIC_LEN &&
        memcmp(plain, RKVC_MODEL_ENC_MAGIC, RKVC_MODEL_ENC_MAGIC_LEN) == 0) {
        fprintf(stderr, "error: %s 已是加密模型，拒绝二次加密\n", in_path);
        free(plain);
        return 1;
    }

    const size_t total = RKVC_MODEL_ENC_HDR_LEN + RKVC_MODEL_ENC_MAC_LEN +
                         plain_len;
    uint8_t *out = malloc(total);
    if (!out) { free(plain); return 1; }

    memcpy(out, RKVC_MODEL_ENC_MAGIC, RKVC_MODEL_ENC_MAGIC_LEN);
    put_le32(out + 8, RKVC_MODEL_ENC_VERSION);
    put_le32(out + 12, 0); /* flags */
    put_le64(out + 16, (uint64_t)plain_len);
    uint8_t *nonce = out + RKVC_MODEL_ENC_HDR_LEN - RKVC_MODEL_ENC_NONCE_LEN;
    randombytes_buf(nonce, RKVC_MODEL_ENC_NONCE_LEN);
    crypto_secretbox_easy(out + RKVC_MODEL_ENC_HDR_LEN, plain, plain_len,
                          nonce, data_key);
    sodium_memzero(data_key, sizeof(data_key));
    sodium_memzero(plain, plain_len);
    free(plain);

    if (write_file(out_path ? out_path : in_path, out, total) != 0) {
        free(out);
        return 1;
    }
    printf("已加密: %s -> %s (%zu -> %zu 字节)\n", in_path,
           out_path ? out_path : in_path, plain_len, total);
    free(out);
    return 0;
}

static int cmd_decrypt(const char *data_key_path, const char *in_path,
                       const char *out_path)
{
    if (!data_key_path || !in_path || !out_path) {
        fprintf(stderr, "用法: rkvc_model_crypt decrypt -d data.key -i <in> -o <out>\n");
        return 2;
    }
    uint8_t data_key[KEY_FILE_LEN];
    if (read_key_file(data_key_path, data_key) != 0)
        return 1;

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (read_model(in_path, &enc, &enc_len) != 0)
        return 1;
    if (enc_len < RKVC_MODEL_ENC_HDR_LEN ||
        memcmp(enc, RKVC_MODEL_ENC_MAGIC, RKVC_MODEL_ENC_MAGIC_LEN) != 0) {
        fprintf(stderr, "error: %s 不是加密模型（无 %s 头）\n",
                in_path, RKVC_MODEL_ENC_MAGIC);
        free(enc);
        return 1;
    }
    const uint64_t plain_len = get_le64(enc + 16);
    if (get_le32(enc + 8) != RKVC_MODEL_ENC_VERSION ||
        enc_len != RKVC_MODEL_ENC_HDR_LEN + RKVC_MODEL_ENC_MAC_LEN + plain_len) {
        fprintf(stderr, "error: %s 头部/长度无效\n", in_path);
        free(enc);
        return 1;
    }
    uint8_t *plain = malloc((size_t)plain_len);
    if (!plain) { free(enc); return 1; }
    if (crypto_secretbox_open_easy(
            plain, enc + RKVC_MODEL_ENC_HDR_LEN,
            RKVC_MODEL_ENC_MAC_LEN + (size_t)plain_len,
            enc + RKVC_MODEL_ENC_HDR_LEN - RKVC_MODEL_ENC_NONCE_LEN,
            data_key) != 0) {
        fprintf(stderr, "error: 解密失败（data.key 不匹配或文件损坏）\n");
        free(plain);
        free(enc);
        return 1;
    }
    sodium_memzero(data_key, sizeof(data_key));
    free(enc);
    if (write_file(out_path, plain, (size_t)plain_len) != 0) {
        free(plain);
        return 1;
    }
    printf("已解密: %s -> %s (%llu 字节)\n", in_path, out_path,
           (unsigned long long)plain_len);
    free(plain);
    return 0;
}

/* ── issue / verify-key ──────────────────────────────────────────── */

static int cmd_issue(const char *data_key_path, const char *master_key_path,
                     const char *machine_hex, const char *out_path)
{
    if (!data_key_path || !master_key_path || !machine_hex || !out_path) {
        fprintf(stderr, "用法: rkvc_model_crypt issue -d data.key -m master.key "
                        "-M <machine_id_hex> -o model.key\n");
        return 2;
    }
    int ret = 1;
    uint8_t data_key[KEY_FILE_LEN] = {0};
    uint8_t master[KEY_FILE_LEN] = {0};
    uint8_t plain[RKVC_MODEL_KEY_PLAIN_LEN] = {0};
    if (read_key_file(data_key_path, data_key) != 0 ||
        read_key_file(master_key_path, master) != 0)
        goto cleanup;

    memcpy(plain, data_key, 32);
    sodium_memzero(data_key, sizeof(data_key));
    if (strlen(machine_hex) != RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN ||
        strspn(machine_hex, "0123456789abcdef") !=
            RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN) {
        fprintf(stderr, "error: 机器码须为 %u 位小写十六进制\n",
                RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN);
        ret = 2;
        goto cleanup;
    }
    memcpy(plain + 32, machine_hex, RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN);

    uint8_t out[RKVC_MODEL_KEY_FILE_LEN];
    memcpy(out, RKVC_MODEL_KEY_MAGIC, RKVC_MODEL_KEY_MAGIC_LEN);
    put_le32(out + 8, RKVC_MODEL_KEY_VERSION);
    uint8_t *nonce = out + 12;
    randombytes_buf(nonce, RKVC_MODEL_ENC_NONCE_LEN);
    crypto_secretbox_easy(out + 12 + RKVC_MODEL_ENC_NONCE_LEN, plain,
                          sizeof(plain), nonce, master);
    if (write_file(out_path, out, sizeof(out)) != 0)
        goto cleanup;
    printf("已签发: %s (绑定机器码 %.8s…，%zu 字节)\n", out_path,
           machine_hex, sizeof(out));
    ret = 0;

cleanup:
    sodium_memzero(data_key, sizeof(data_key));
    sodium_memzero(master, sizeof(master));
    sodium_memzero(plain, sizeof(plain));
    return ret;
}

static int cmd_verify_key(const char *master_key_path, const char *key_path)
{
    if (!master_key_path || !key_path) {
        fprintf(stderr, "用法: rkvc_model_crypt verify-key -m master.key -f model.key\n");
        return 2;
    }
    uint8_t master[KEY_FILE_LEN];
    if (read_key_file(master_key_path, master) != 0)
        return 1;

    uint8_t *enc = NULL;
    size_t enc_len = 0;
    if (read_model(key_path, &enc, &enc_len) != 0)
        return 1;
    if (enc_len != RKVC_MODEL_KEY_FILE_LEN ||
        memcmp(enc, RKVC_MODEL_KEY_MAGIC, RKVC_MODEL_KEY_MAGIC_LEN) != 0 ||
        get_le32(enc + 8) != RKVC_MODEL_KEY_VERSION) {
        fprintf(stderr, "error: %s 不是有效的 model.key\n", key_path);
        free(enc);
        return 1;
    }
    uint8_t plain[RKVC_MODEL_KEY_PLAIN_LEN];
    if (crypto_secretbox_open_easy(
            plain, enc + 12 + RKVC_MODEL_ENC_NONCE_LEN,
            RKVC_MODEL_ENC_MAC_LEN + sizeof(plain), enc + 12, master) != 0) {
        fprintf(stderr, "error: model.key MAC 校验失败（master.key 不匹配）\n");
        free(enc);
        return 1;
    }
    free(enc);
    char hex[RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN + 1];
    memcpy(hex, plain + 32, RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN);
    hex[sizeof(hex) - 1] = '\0';
    printf("model.key 有效，绑定机器码: %s\n", hex);
    sodium_memzero(plain, sizeof(plain));
    sodium_memzero(master, sizeof(master));
    return 0;
}

static int cmd_machine_id(void)
{
    char hex[LIC_MACHINE_ID_HEX_LEN];   /* 含 NUL，见上方 _Static_assert */
    if (lic_machine_id_hex(hex, sizeof(hex)) != 0) {
        fprintf(stderr, "错误: 无法采集本机机器码（无可用硬件指纹）\n");
        return 1;
    }
    printf("%s\n", hex);
    return 0;
}

int main(int argc, char **argv)
{
    if (sodium_init() < 0) {
        fprintf(stderr, "error: sodium_init failed\n");
        return 1;
    }
    if (argc < 2) {
        fprintf(stderr, "用法: rkvc_model_crypt <genkey|encrypt|decrypt|issue|verify-key> ...\n");
        return 2;
    }
    const char *cmd = argv[1];
    const char *in = NULL, *out = NULL, *data_key = NULL,
               *master_key = NULL, *machine = NULL, *file = NULL;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) in = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) data_key = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) master_key = argv[++i];
        else if (!strcmp(argv[i], "-M") && i + 1 < argc) machine = argv[++i];
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) file = argv[++i];
        else {
            fprintf(stderr, "error: 未知参数 %s\n", argv[i]);
            return 2;
        }
    }

    if (!strcmp(cmd, "genkey"))
        return cmd_genkey(out);
    if (!strcmp(cmd, "encrypt"))
        return cmd_encrypt(data_key, in, out);
    if (!strcmp(cmd, "decrypt"))
        return cmd_decrypt(data_key, in, out);
    if (!strcmp(cmd, "issue"))
        return cmd_issue(data_key, master_key, machine, out);
    if (!strcmp(cmd, "verify-key"))
        return cmd_verify_key(master_key, file);
    if (!strcmp(cmd, "machine-id"))
        return cmd_machine_id();
    fprintf(stderr, "error: 未知子命令 %s\n", cmd);
    return 2;
}
