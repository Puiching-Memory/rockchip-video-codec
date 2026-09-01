/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file model_trust.c
 * @brief .rkmodel 签名验证：Ed25519 + dev/prod trust root 分离。
 *
 * trust root 在编译期固定（RKVC_TRUST_PUBKEY_HEX；prod 构建用
 * RKVC_TRUST_PRODUCTION=1 且 unsigned 模型视为 untrusted）。key_id 取
 * SHA-256(pubkey) 前 16 字节。验证覆盖固定头+TLV+载荷表（含各载荷摘要），
 * 故签名间接覆盖全部载荷。
 *
 * 仅在 RKVC_ENABLE_MODEL_SIGN 时编入；否则 rkvc_model_trust_verifier()
 * 返回 NULL（已签名模型标 untrusted，如实上报）。
 */

#include "rkmodel.h"

#include <string.h>

#ifdef RKVC_ENABLE_MODEL_SIGN

#include <sodium.h>

static uint8_t g_root_pk[32]; /**< 编译期 trust root 公钥（首次使用时解析） */
static int     g_root_set = 0; /**< g_root_pk 是否已初始化 */

/** 十六进制串解到 n 字节；非法字符返回 -1。 */
static int hex_to_bytes(const char *hex, uint8_t *out, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/** 惰性解析 RKVC_TRUST_PUBKEY_HEX 到 g_root_pk（仅一次）。 */
static void ensure_root(void) {
    if (g_root_set)
        return;
#ifdef RKVC_TRUST_PUBKEY_HEX
    if (hex_to_bytes(RKVC_TRUST_PUBKEY_HEX, g_root_pk, 32) == 0)
        g_root_set = 1;
#endif
    (void)g_root_pk;
}

#ifdef RKVC_STANDALONE_TEST
/** 测试钩子：直接注入 trust root 公钥（绕过编译期常量）。 */
void rkvc_model_trust_install_root_for_test(const uint8_t pk[32]) {
    memcpy(g_root_pk, pk, 32);
    g_root_set = 1;
}
#endif

int rkvc_model_trust_production_mode(void) {
#ifdef RKVC_TRUST_PRODUCTION
    return RKVC_TRUST_PRODUCTION;
#else
    return 0;
#endif
}

/** Ed25519 验签回调：key_id 须匹配 SHA-256(pubkey) 前 16 字节。 */
static int verify_ed25519(const uint8_t key_id[16], const uint8_t sig[64],
                          const uint8_t *bytes, size_t len,
                          rkvc_model_trust *trust, void *opaque) {
    crypto_hash_sha256_state st;
    uint8_t id[32];
    (void)opaque;

    ensure_root();
    if (!g_root_set)
        return -1;
    crypto_hash_sha256_init(&st);
    crypto_hash_sha256_update(&st, g_root_pk, sizeof(g_root_pk));
    crypto_hash_sha256_final(&st, id);
    if (memcmp(id, key_id, 16) != 0)
        return -1;
    if (sodium_init() < 0)
        return -1;
    if (crypto_sign_ed25519_verify_detached(sig, bytes, len, g_root_pk) != 0)
        return -1;
    *trust = rkvc_model_trust_production_mode()
                 ? RKVC_MODEL_TRUST_PRODUCTION
                 : RKVC_MODEL_TRUST_DEVELOPMENT;
    return 0;
}

rkvc_rkmodel_verify_fn rkvc_model_trust_verifier(void) {
    return verify_ed25519;
}

#else /* !RKVC_ENABLE_MODEL_SIGN */

int rkvc_model_trust_production_mode(void) { return 0; }

rkvc_rkmodel_verify_fn rkvc_model_trust_verifier(void) { return NULL; }

#endif /* RKVC_ENABLE_MODEL_SIGN */
