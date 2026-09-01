/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_model_trust.c
 * @brief .rkmodel Ed25519 签名验证回归（独立编译 + libsodium）。
 *
 * 用 libsodium 生成密钥对并签名（与 rkmodel.py sign 同语义：flags 置位、
 * 载荷偏移重排、签名覆盖固定头+TLV+载荷表），经真实验证器走 dev/prod 模式。
 */

#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cmocka.h>
#include <sodium.h>

#include "rkmodel.h"

void rkvc_model_trust_install_root_for_test(const uint8_t pk[32]);

/* ── 容器构造 + 签名（镜像 rkmodel.py sign 语义） ─────────────────── */

typedef struct { uint8_t *b; size_t len, cap; } blob;
static void bp(blob *x, const void *p, size_t n) {
    if (x->len + n > x->cap) { x->cap = (x->len + n) * 2 + 128; x->b = realloc(x->b, x->cap); }
    memcpy(x->b + x->len, p, n); x->len += n;
}
static void bu16(blob *x, uint16_t v) { bp(x, &v, 2); }
static void bu32(blob *x, uint32_t v) { bp(x, &v, 4); }
static void bu64(blob *x, uint64_t v) { bp(x, &v, 8); }
static void btlv(blob *x, uint16_t tag, const char *s) {
    bu16(x, tag); bu32(x, (uint32_t)strlen(s)); bp(x, s, strlen(s));
}

static const uint8_t PAYLOAD[257] = { [0] = 0x11, [256] = 0xee };

/** 构造已签名容器：payloads 位于签名尾之后。 */
static void build_signed(blob *out, const uint8_t sk[64],
                         const uint8_t pk[32]) {
    blob tlv = {0}, table = {0};
    rkmodel_fixed fx = {0};
    rkmodel_sig_trailer tr;
    blob sig_input = {0};
    size_t data_off;
    uint8_t digest[32];

    btlv(&tlv, RKMODEL_TAG_FAMILY, "sr");
    btlv(&tlv, RKMODEL_TAG_ROLE, "upscale");
    btlv(&tlv, RKMODEL_TAG_ID, "sr-x2-signed");
    btlv(&tlv, RKMODEL_TAG_VERSION, "2.0.0");

    data_off = RKMODEL_FIXED_SIZE + tlv.len + sizeof(rkmodel_payload_entry) +
               sizeof(tr);
    {
        crypto_hash_sha256_state st;
        crypto_hash_sha256_init(&st);
        crypto_hash_sha256_update(&st, PAYLOAD, sizeof(PAYLOAD));
        crypto_hash_sha256_final(&st, digest);
    }
    bu32(&table, RKMODEL_PAYLOAD_RKNN);
    bu32(&table, 0);
    bu64(&table, (uint64_t)data_off);
    bu64(&table, (uint64_t)sizeof(PAYLOAD));
    bp(&table, digest, 32);

    fx.magic = RKMODEL_MAGIC;
    fx.format_version = RKMODEL_VERSION;
    fx.header_len = (uint32_t)tlv.len;
    fx.payload_count = 1;
    fx.flags = RKMODEL_FLAG_SIGNED;

    bp(&sig_input, &fx, sizeof(fx));
    bp(&sig_input, tlv.b, tlv.len);
    bp(&sig_input, table.b, table.len);

    memset(&tr, 0, sizeof(tr));
    tr.alg = RKMODEL_SIG_ED25519;
    {
        uint8_t id[32];
        crypto_hash_sha256_state st;
        crypto_hash_sha256_init(&st);
        crypto_hash_sha256_update(&st, pk, 32);
        crypto_hash_sha256_final(&st, id);
        memcpy(tr.key_id, id, 16);
    }
    crypto_sign_ed25519_detached(tr.sig, NULL, sig_input.b, sig_input.len, sk);

    memset(out, 0, sizeof(*out));
    bp(out, sig_input.b, sig_input.len);
    bp(out, &tr, sizeof(tr));
    bp(out, PAYLOAD, sizeof(PAYLOAD));
    free(tlv.b); free(table.b); free(sig_input.b);
}

static void write_blob(const char *path, const blob *c) {
    FILE *f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(c->b, 1, c->len, f), c->len);
    fclose(f);
}

static uint8_t g_pk[32], g_sk[64];

static void test_signed_model_verified(void **state) {
    blob c;
    rkvc_rkmodel m;
    FILE *f;
    (void)state;

    rkvc_model_trust_install_root_for_test(g_pk);
    build_signed(&c, g_sk, g_pk);
    write_blob("/tmp/rkvc_test_trust.rkmodel", &c);

    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_trust.rkmodel", &m,
                                       rkvc_model_trust_verifier(), NULL,
                                       NULL, 0),
                     RKVC_STATUS_OK);
    /* dev 模式 → development；prod 模式 → production（由编译定义决定） */
    if (rkvc_model_trust_production_mode())
        assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_PRODUCTION);
    else
        assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_DEVELOPMENT);

    f = fopen("/tmp/rkvc_test_trust.rkmodel", "rb");
    assert_int_equal(rkvc_rkmodel_check_payload(f, &m, RKMODEL_PAYLOAD_RKNN),
                     RKVC_STATUS_OK);
    fclose(f);
    free(c.b);
}

static void test_forged_signature_rejected(void **state) {
    blob c;
    rkvc_rkmodel m;
    size_t sig_off;
    (void)state;

    build_signed(&c, g_sk, g_pk);
    /* 翻转签名尾中 sig 的一字节 */
    sig_off = RKMODEL_FIXED_SIZE;
    {
        rkmodel_fixed *fx = (rkmodel_fixed *)c.b;
        sig_off += fx->header_len + sizeof(rkmodel_payload_entry) + 4 + 16;
    }
    c.b[sig_off] ^= 0x01;
    write_blob("/tmp/rkvc_test_forged.rkmodel", &c);

    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_forged.rkmodel", &m,
                                       rkvc_model_trust_verifier(), NULL,
                                       NULL, 0),
                     RKVC_STATUS_OK);
    assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_UNTRUSTED);
    free(c.b);
}

static void test_wrong_key_rejected(void **state) {
    blob c;
    rkvc_rkmodel m;
    uint8_t other_pk[32], other_sk[64];
    (void)state;

    crypto_sign_ed25519_keypair(other_pk, other_sk);
    build_signed(&c, other_sk, other_pk); /* 非信任根签名 */
    write_blob("/tmp/rkvc_test_wrongkey.rkmodel", &c);

    assert_int_equal(rkvc_rkmodel_open("/tmp/rkvc_test_wrongkey.rkmodel", &m,
                                       rkvc_model_trust_verifier(), NULL,
                                       NULL, 0),
                     RKVC_STATUS_OK);
    assert_int_equal(m.info.trust, RKVC_MODEL_TRUST_UNTRUSTED);
    free(c.b);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_signed_model_verified),
        cmocka_unit_test(test_forged_signature_rejected),
        cmocka_unit_test(test_wrong_key_rejected),
    };
    if (sodium_init() < 0)
        return 1;
    crypto_sign_ed25519_keypair(g_pk, g_sk);
    return cmocka_run_group_tests(tests, NULL, NULL);
}
