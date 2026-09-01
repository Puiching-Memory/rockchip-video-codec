/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_rkmodel.c
 * @brief .rkmodel v1 容器与模型注册表回归测试（独立编译，cmocka）。
 *
 * 覆盖：往返读写、载荷摘要校验、坏 magic、越界 header_len、未知 TLV 跳过、
 * 签名尾（无验证后端→untrusted；假验证器→development）、注册表扫描过滤。
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <cmocka.h>
#include <sodium.h>

#include "rkmodel.h"
#include "context_internal.h"

/* ── 测试侧容器构造（与 rkmodel.py 写入侧同格式） ────────────────── */

typedef struct {
    uint8_t *bytes;
    size_t   len;
    size_t   cap;
} blob;

static void blob_put(blob *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 128;
        b->bytes = realloc(b->bytes, b->cap);
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void blob_u16(blob *b, uint16_t v) { blob_put(b, &v, 2); }
static void blob_u32(blob *b, uint32_t v) { blob_put(b, &v, 4); }
static void blob_u64(blob *b, uint64_t v) { blob_put(b, &v, 8); }

static void blob_tlv(blob *b, uint16_t tag, const char *s) {
    blob_u16(b, tag);
    blob_u32(b, (uint32_t)strlen(s));
    blob_put(b, s, strlen(s));
}

static const uint8_t PAYLOAD_A[97] = { [0] = 0x42, [96] = 0x24 };
static const uint8_t PAYLOAD_B[53] = { [0] = 0x7e, [52] = 0xe7 };

typedef struct {
    uint32_t kind;
    const uint8_t *data;
    size_t len;
} test_payload;

/** 构造容器字节流；flags 由调用方控制（签名尾数据另附）。 */
static void build_container(blob *out, uint16_t extra_tlv_tag,
                            const test_payload *pl, size_t npl, uint32_t flags,
                            const uint8_t *sig_trailer) {
    blob tlv = {0}, table = {0}, body = {0};
    rkmodel_fixed fx = {0};
    size_t table_off, i;

    blob_tlv(&tlv, RKMODEL_TAG_FAMILY, "sr");
    blob_tlv(&tlv, RKMODEL_TAG_ROLE, "upscale");
    blob_tlv(&tlv, RKMODEL_TAG_ID, "sr-x2-test");
    blob_tlv(&tlv, RKMODEL_TAG_VERSION, "1.0.0");
    blob_tlv(&tlv, RKMODEL_TAG_RKNN_TARGET, "rk3588");
    if (extra_tlv_tag) { /* 未知 tag 应被跳过 */
        blob_u16(&tlv, extra_tlv_tag);
        blob_u32(&tlv, 4);
        blob_u32(&tlv, 0xdeadbeef);
    }

    table_off = RKMODEL_FIXED_SIZE + tlv.len + npl * sizeof(rkmodel_payload_entry);
    {
        size_t data_off = table_off;
        for (i = 0; i < npl; ++i) {
            crypto_hash_sha256_state st;
            uint8_t digest[32];
            blob_u32(&table, pl[i].kind);
            blob_u32(&table, 0);
            blob_u64(&table, (uint64_t)data_off);
            blob_u64(&table, (uint64_t)pl[i].len);
            crypto_hash_sha256_init(&st);
            crypto_hash_sha256_update(&st, pl[i].data, pl[i].len);
            crypto_hash_sha256_final(&st, digest);
            blob_put(&table, digest, 32);
            data_off += pl[i].len;
        }
        for (i = 0; i < npl; ++i)
            blob_put(&body, pl[i].data, pl[i].len);
    }

    fx.magic = RKMODEL_MAGIC;
    fx.format_version = RKMODEL_VERSION;
    fx.header_len = (uint32_t)tlv.len;
    fx.payload_count = (uint32_t)npl;
    fx.flags = flags;

    memset(out, 0, sizeof(*out));
    blob_put(out, &fx, sizeof(fx));
    blob_put(out, tlv.bytes, tlv.len);
    blob_put(out, table.bytes, table.len);
    if (sig_trailer)
        blob_put(out, sig_trailer, sizeof(rkmodel_sig_trailer));
    blob_put(out, body.bytes, body.len);

    free(tlv.bytes);
    free(table.bytes);
    free(body.bytes);
}

static void write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(p, 1, n, f), n);
    fclose(f);
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

static void test_roundtrip(void **state) {
    (void)state;
    test_payload pls[2] = {
        { RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A) },
        { RKMODEL_PAYLOAD_PMF, PAYLOAD_B, sizeof(PAYLOAD_B) },
    };
    blob c;
    rkvc_rkmodel m;
    char err[160] = {0};
    FILE *f;

    build_container(&c, 0x77 /* 未知 tag */, pls, 2, 0, NULL);
    write_file("/tmp/rkvc_test_roundtrip.rkmodel", c.bytes, c.len);

    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_roundtrip.rkmodel", &m,
                                       NULL, NULL, err, sizeof(err)),
                     RKVC_STATUS_OK);
    assert_string_equal(m.info.id, "sr-x2-test");
    assert_string_equal(m.info.family, "sr");
    assert_string_equal(m.info.role, "upscale");
    assert_string_equal(m.info.version, "1.0.0");
    assert_string_equal(m.info.rknn_target, "rk3588");
    assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_UNSIGNED);
    assert_int_equal(m.payload_count, 2);
    assert_true(m.info.payload_mask & (1u << RKMODEL_PAYLOAD_RKNN));
    assert_true(m.info.payload_mask & (1u << RKMODEL_PAYLOAD_PMF));

    f = fopen("/tmp/rkvc_test_roundtrip.rkmodel", "rb");
    assert_non_null(f);
    assert_int_equal(rkvc_rkmodel_check_payload(f, &m, RKMODEL_PAYLOAD_RKNN),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_rkmodel_check_payload(f, &m, RKMODEL_PAYLOAD_PMF),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_rkmodel_check_payload(f, &m, RKMODEL_PAYLOAD_QPPATCH),
                     RKVC_STATUS_NOT_FOUND);
    fclose(f);
    free(c.bytes);
}

static void test_tampered_payload(void **state) {
    (void)state;
    test_payload pls[1] = { { RKMODEL_PAYLOAD_RKNN, PAYLOAD_A, sizeof(PAYLOAD_A) } };
    blob c;
    rkvc_rkmodel m;
    FILE *f;

    build_container(&c, 0, pls, 1, 0, NULL);
    c.bytes[c.len - 1] ^= 0xff; /* 翻转载荷最后一字节 */
    write_file("/tmp/rkvc_test_tamper.rkmodel", c.bytes, c.len);

    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_tamper.rkmodel", &m,
                                       NULL, NULL, NULL, 0),
                     RKVC_STATUS_OK); /* 头部仍有效 */
    f = fopen("/tmp/rkvc_test_tamper.rkmodel", "rb");
    assert_int_equal(rkvc_rkmodel_check_payload(f, &m, RKMODEL_PAYLOAD_RKNN),
                     RKVC_STATUS_INTEGRITY);
    fclose(f);
    free(c.bytes);
}

static void test_bad_magic(void **state) {
    (void)state;
    test_payload pls[1] = { { RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B) } };
    blob c;
    rkvc_rkmodel m;
    char err[160] = {0};

    build_container(&c, 0, pls, 1, 0, NULL);
    c.bytes[0] ^= 0xff;
    write_file("/tmp/rkvc_test_badmagic.rkmodel", c.bytes, c.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_badmagic.rkmodel", &m,
                                       NULL, NULL, err, sizeof(err)),
                     RKVC_STATUS_INVALID);
    assert_non_null(strstr(err, "magic"));
    free(c.bytes);
}

static void test_oversized_header(void **state) {
    (void)state;
    test_payload pls[1] = { { RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B) } };
    blob c;
    rkvc_rkmodel m;
    uint32_t huge = RKMODEL_MAX_HEADER + 1;

    build_container(&c, 0, pls, 1, 0, NULL);
    memcpy(c.bytes + offsetof(rkmodel_fixed, header_len), &huge, 4);
    write_file("/tmp/rkvc_test_hugehdr.rkmodel", c.bytes, c.len);
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_hugehdr.rkmodel", &m,
                                       NULL, NULL, NULL, 0),
                     RKVC_STATUS_INVALID);
    free(c.bytes);
}

static int fake_verify_ok(const uint8_t key_id[16], const uint8_t sig[64],
                          const uint8_t *bytes, size_t len,
                          rkvc_model_trust *trust, void *opaque) {
    (void)key_id; (void)sig; (void)bytes; (void)len; (void)opaque;
    *trust = RKVC_MODEL_TRUST_DEVELOPMENT;
    return 0;
}

static void test_signed_no_verifier(void **state) {
    (void)state;
    test_payload pls[1] = { { RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B) } };
    blob c;
    rkvc_rkmodel m;
    rkmodel_sig_trailer tr;

    memset(&tr, 0, sizeof(tr));
    tr.alg = RKMODEL_SIG_ED25519;
    build_container(&c, 0, pls, 1, RKMODEL_FLAG_SIGNED, (const uint8_t *)&tr);
    write_file("/tmp/rkvc_test_signed.rkmodel", c.bytes, c.len);

    /* 无验证后端：untrusted */
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_signed.rkmodel", &m,
                                       NULL, NULL, NULL, 0),
                     RKVC_STATUS_OK);
    assert_true(m.has_signature);
    assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_UNTRUSTED);

    /* 有验证器：信任级别来自验证器 */
    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_signed.rkmodel", &m,
                                       fake_verify_ok, NULL, NULL, 0),
                     RKVC_STATUS_OK);
    assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_DEVELOPMENT);
    free(c.bytes);
}

static void test_registry_scan(void **state) {
    (void)state;
    test_payload pls[1] = { { RKMODEL_PAYLOAD_RKNN, PAYLOAD_B, sizeof(PAYLOAD_B) } };
    blob good, bad;
    const char *dirs[1] = { "/tmp/rkvc_test_models" };
    rkvc_context_options opts;
    rkvc_context *ctx = NULL;
    rkvc_model_info mi;

    mkdir("/tmp/rkvc_test_models", 0755);
    build_container(&good, 0, pls, 1, 0, NULL);
    write_file("/tmp/rkvc_test_models/good.rkmodel", good.bytes, good.len);
    build_container(&bad, 0, pls, 1, 0, NULL);
    bad.bytes[0] ^= 0xff;
    write_file("/tmp/rkvc_test_models/bad.rkmodel", bad.bytes, bad.len);
    write_file("/tmp/rkvc_test_models/notamodel.txt", (const uint8_t *)"x", 1);

    rkvc_context_options_init(&opts, sizeof(opts));
    opts.paths.model_dirs = dirs;
    opts.paths.model_dir_count = 1;
    assert_int_equal(rkvc_context_create(&opts, &ctx), RKVC_STATUS_OK);
    assert_non_null(ctx);
    assert_int_equal(rkvc_model_count(ctx), 1);   /* 坏候选被淘汰 */
    assert_int_equal(rkvc_model_info_at(ctx, 0, &mi), RKVC_STATUS_OK);
    assert_string_equal(mi.id, "sr-x2-test");
    assert_int_equal(rkvc_model_info_at(ctx, 1, &mi), RKVC_STATUS_NOT_FOUND);
    rkvc_context_destroy(ctx);
    free(good.bytes);
    free(bad.bytes);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roundtrip),
        cmocka_unit_test(test_tampered_payload),
        cmocka_unit_test(test_bad_magic),
        cmocka_unit_test(test_oversized_header),
        cmocka_unit_test(test_signed_no_verifier),
        cmocka_unit_test(test_registry_scan),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
