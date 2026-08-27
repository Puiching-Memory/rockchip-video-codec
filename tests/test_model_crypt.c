/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_model_crypt.c
 * @brief 模型自研加密层单测：格式解析、解密 round-trip、机器码绑定、防篡改。
 *
 * 测试内扮演「打包方」：用 lib 内嵌主密钥（演示密钥）与随机 data_key
 * 构造加密模型与 model.key，再走运行时解密路径验证。
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sodium.h>

#include "rkvc/rkvc.h"
#include "internal.h"
#include "license_machine.h"
#include "model_crypt_layout.h"

/** lib/model_key.c（演示）或 CMake 生成文件提供 */
extern const uint8_t rkvc_model_masterkey_enc[32];

static char tmpdir[256];

static void master_key_deobf(uint8_t out[32])
{
    for (unsigned i = 0; i < 32; i++)
        out[i] = rkvc_model_masterkey_enc[i] ^ (uint8_t)(0xA5 ^ (i * 7));
}

static void wr_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr_u64le(uint8_t *p, uint64_t v)
{
    wr_u32le(p, (uint32_t)v);
    wr_u32le(p + 4, (uint32_t)(v >> 32));
}

static void write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(data, 1, len, f), len);
    fclose(f);
}

/** 打包方视角：用 data_key 加密明文模型 */
static size_t build_encrypted_model(const uint8_t *plain, size_t plain_len,
                                    const uint8_t data_key[32], uint8_t **out)
{
    const size_t total = RKVC_MODEL_ENC_HDR_LEN + RKVC_MODEL_ENC_MAC_LEN +
                         plain_len;
    uint8_t *buf = malloc(total);
    assert_non_null(buf);
    memcpy(buf, RKVC_MODEL_ENC_MAGIC, RKVC_MODEL_ENC_MAGIC_LEN);
    wr_u32le(buf + 8, RKVC_MODEL_ENC_VERSION);
    wr_u32le(buf + 12, 0);
    wr_u64le(buf + 16, (uint64_t)plain_len);
    uint8_t *nonce = buf + RKVC_MODEL_ENC_HDR_LEN - RKVC_MODEL_ENC_NONCE_LEN;
    randombytes_buf(nonce, RKVC_MODEL_ENC_NONCE_LEN);
    crypto_secretbox_easy(buf + RKVC_MODEL_ENC_HDR_LEN, plain, plain_len,
                          nonce, data_key);
    *out = buf;
    return total;
}

/** 打包方视角：用 master 密封 data_key + 机器码 → model.key */
static void build_model_key(const uint8_t data_key[32],
                            const char *machine_hex, const char *path)
{
    uint8_t master[32];
    master_key_deobf(master);
    uint8_t plain[RKVC_MODEL_KEY_PLAIN_LEN];
    memcpy(plain, data_key, 32);
    memcpy(plain + 32, machine_hex, RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN);

    uint8_t out[RKVC_MODEL_KEY_FILE_LEN];
    memcpy(out, RKVC_MODEL_KEY_MAGIC, RKVC_MODEL_KEY_MAGIC_LEN);
    wr_u32le(out + 8, RKVC_MODEL_KEY_VERSION);
    randombytes_buf(out + 12, RKVC_MODEL_ENC_NONCE_LEN);
    crypto_secretbox_easy(out + 12 + RKVC_MODEL_ENC_NONCE_LEN, plain,
                          sizeof(plain), out + 12, master);
    write_file(path, out, sizeof(out));
    sodium_memzero(master, sizeof(master));
}

static void set_key_env(const char *path)
{
    assert_int_equal(setenv("RKVC_MODEL_KEY_FILE", path, 1), 0);
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

static void test_is_encrypted(void **state)
{
    (void)state;
    assert_false(rkvc_model_crypt_is_encrypted(NULL, 0));
    assert_false(rkvc_model_crypt_is_encrypted("RKVCENC1", 4));
    assert_true(rkvc_model_crypt_is_encrypted("RKVCENC1", 8));
    assert_false(rkvc_model_crypt_is_encrypted("RKNN....", 8));
}

static void test_plaintext_passthrough(void **state)
{
    (void)state;
    const uint8_t plain[] = "plain rknn model bytes \x01\x02\x03";
    char path[320];
    snprintf(path, sizeof(path), "%s/plain.rknn", tmpdir);
    write_file(path, plain, sizeof(plain));

    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(path, &buf, &size), RKVC_OK);
    assert_int_equal(size, sizeof(plain));
    assert_memory_equal(buf, plain, sizeof(plain));
    rkvc_free(buf);
}

static void test_missing_model_key(void **state)
{
    (void)state;
    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    const uint8_t plain[] = "encrypted model payload";
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char path[320];
    snprintf(path, sizeof(path), "%s/no_key.rknn", tmpdir);
    write_file(path, enc, enc_len);
    free(enc);

    set_key_env("/nonexistent/rkvc-test-model.key");
    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(path, &buf, &size),
                     RKVC_ERR_UNLICENSED);
}

static void test_roundtrip_with_local_machine(void **state)
{
    (void)state;
    char machine_hex[LIC_MACHINE_ID_HEX_LEN];
    if (lic_machine_id_hex(machine_hex, sizeof(machine_hex)) != 0)
        skip(); /* 无可用硬件指纹的环境（如纯净容器）跳过绑定用例 */

    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    uint8_t plain[10000];
    randombytes_buf(plain, sizeof(plain));
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char model_path[320], key_path[320];
    snprintf(model_path, sizeof(model_path), "%s/rt.rknn", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/rt.key", tmpdir);
    write_file(model_path, enc, enc_len);
    free(enc);
    build_model_key(data_key, machine_hex, key_path);

    set_key_env(key_path);
    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(model_path, &buf, &size),
                     RKVC_OK);
    assert_int_equal(size, sizeof(plain));
    assert_memory_equal(buf, plain, sizeof(plain));
    rkvc_free(buf);
}

static void test_machine_mismatch(void **state)
{
    (void)state;
    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    const uint8_t plain[] = "bound to another machine";
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char model_path[320], key_path[320];
    snprintf(model_path, sizeof(model_path), "%s/mm.rknn", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/mm.key", tmpdir);
    write_file(model_path, enc, enc_len);
    free(enc);
    /* 全 0 机器码：不可能与本机指纹一致（指纹为 SHA-256 hex） */
    char fake_hex[RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN + 1];
    memset(fake_hex, '0', sizeof(fake_hex) - 1);
    fake_hex[sizeof(fake_hex) - 1] = '\0';
    build_model_key(data_key, fake_hex, key_path);

    /* 本机无指纹时解密会先在机器码采集处失败，同样返回 LICENSE */
    set_key_env(key_path);
    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(model_path, &buf, &size),
                     RKVC_ERR_LICENSE);
}

static void test_wrong_data_key(void **state)
{
    (void)state;
    uint8_t data_key[32], wrong_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    randombytes_buf(wrong_key, sizeof(wrong_key));
    const uint8_t plain[] = "wrong key payload";
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char model_path[320], key_path[320];
    snprintf(model_path, sizeof(model_path), "%s/wk.rknn", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/wk.key", tmpdir);
    write_file(model_path, enc, enc_len);
    free(enc);
    /* model.key 携带错误 data_key；绑定伪造机器码：机器码不符返回 LICENSE，
     * 本机无指纹时采集失败同样返回 LICENSE，两条路径预期一致。 */
    char fake_hex[RKVC_MODEL_CRYPT_MACHINE_ID_HEX_LEN + 1];
    memset(fake_hex, 'a', sizeof(fake_hex) - 1);
    fake_hex[sizeof(fake_hex) - 1] = '\0';
    build_model_key(wrong_key, fake_hex, key_path);

    set_key_env(key_path);
    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(model_path, &buf, &size),
                     RKVC_ERR_LICENSE);
}

static void test_tampered_ciphertext(void **state)
{
    (void)state;
    char machine_hex[LIC_MACHINE_ID_HEX_LEN];
    if (lic_machine_id_hex(machine_hex, sizeof(machine_hex)) != 0)
        skip(); /* 需本机指纹：先验证正常解密，再篡改验证 MAC 拒绝 */

    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    uint8_t plain[64];
    randombytes_buf(plain, sizeof(plain));
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char model_path[320], key_path[320];
    snprintf(model_path, sizeof(model_path), "%s/tamper.rknn", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/tamper.key", tmpdir);
    build_model_key(data_key, machine_hex, key_path);
    set_key_env(key_path);

    /* 未篡改时解密成功 */
    write_file(model_path, enc, enc_len);
    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(model_path, &buf, &size),
                     RKVC_OK);
    rkvc_free(buf);

    /* 篡改最后一个密文字节 → MAC 失败 */
    enc[enc_len - 1] ^= 0xFF;
    write_file(model_path, enc, enc_len);
    buf = NULL;
    assert_int_equal(rkvc_model_crypt_load_file(model_path, &buf, &size),
                     RKVC_ERR_LICENSE);
    free(enc);
}

static void test_truncated_file(void **state)
{
    (void)state;
    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    uint8_t plain[128];
    randombytes_buf(plain, sizeof(plain));
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    char path[320];
    snprintf(path, sizeof(path), "%s/trunc.rknn", tmpdir);
    write_file(path, enc, enc_len - 8); /* 截断尾部 */
    free(enc);

    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(path, &buf, &size),
                     RKVC_ERR_FORMAT);
}

static void test_bad_header(void **state)
{
    (void)state;
    uint8_t data_key[32];
    randombytes_buf(data_key, sizeof(data_key));
    const uint8_t plain[] = "bad header payload";
    uint8_t *enc = NULL;
    const size_t enc_len = build_encrypted_model(plain, sizeof(plain),
                                                 data_key, &enc);
    wr_u32le(enc + 8, 99); /* 不支持的版本 */
    char path[320];
    snprintf(path, sizeof(path), "%s/badver.rknn", tmpdir);
    write_file(path, enc, enc_len);
    free(enc);

    void *buf = NULL;
    size_t size = 0;
    assert_int_equal(rkvc_model_crypt_load_file(path, &buf, &size),
                     RKVC_ERR_FORMAT);

    /* flags 非 0 同样拒绝 */
    uint8_t *enc2 = NULL;
    const size_t enc2_len = build_encrypted_model(plain, sizeof(plain),
                                                  data_key, &enc2);
    wr_u32le(enc2 + 12, RKVC_MODEL_ENC_FLAG_RESERVED);
    char path2[320];
    snprintf(path2, sizeof(path2), "%s/badflags.rknn", tmpdir);
    write_file(path2, enc2, enc2_len);
    free(enc2);
    assert_int_equal(rkvc_model_crypt_load_file(path2, &buf, &size),
                     RKVC_ERR_FORMAT);
}

static int setup(void **state)
{
    (void)state;
    if (sodium_init() < 0)
        return -1;
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/rkvc-model-crypt-test-%d",
             (int)getpid());
    if (mkdir(tmpdir, 0700) != 0)
        return -1;
    return 0;
}

static int teardown(void **state)
{
    (void)state;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpdir);
    if (system(cmd) != 0)
        return -1;
    return 0;
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_is_encrypted),
        cmocka_unit_test(test_plaintext_passthrough),
        cmocka_unit_test(test_missing_model_key),
        cmocka_unit_test(test_roundtrip_with_local_machine),
        cmocka_unit_test(test_machine_mismatch),
        cmocka_unit_test(test_wrong_data_key),
        cmocka_unit_test(test_tampered_ciphertext),
        cmocka_unit_test(test_truncated_file),
        cmocka_unit_test(test_bad_header),
    };
    return cmocka_run_group_tests(tests, setup, teardown);
}
