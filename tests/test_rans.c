/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * rANS 熵编解码回归：双变体 round-trip（含 bypass 离群值与负索引契约）、
 * 流式多段编解码、码流确定性与错误路径。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>

#include "rans.h"

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* symbol_bits=4 → 每分布频率和 = 16；每分布长度含 bypass 哨兵（≥2）。 */
static const int32_t pmf_lengths[] = { 6, 3, 5 };
static const int32_t pmf_offsets[] = { 2, -1, 0 };
static const int32_t pmf_table[] = {
    1, 2, 3, 4, 5, 1,   /* 分布0：和=16 */
    7, 8, 1,            /* 分布1：和=16 */
    4, 4, 4, 3, 1,      /* 分布2：和=16 */
};
#define NUM_DISTS 3
#define TABLE_SIZE (sizeof(pmf_table) / sizeof(pmf_table[0]))
#define N_VALUES 100000

static void roundtrip_variant(void **state, rkvc_rans_variant variant)
{
    (void)state;
    rng_state = 0xDEADBEEFu;

    rkvc_rans_coder coder;
    int rc = rkvc_rans_coder_init(&coder, variant,
                                  pmf_lengths, NUM_DISTS,
                                  pmf_offsets, NUM_DISTS,
                                  pmf_table, TABLE_SIZE, 4, 2);
    assert_int_equal(rc, RKVC_RANS_OK);

    int32_t *idx = malloc(N_VALUES * sizeof(int32_t));
    int32_t *val = malloc(N_VALUES * sizeof(int32_t));
    int32_t *dec = malloc(N_VALUES * sizeof(int32_t));
    assert_non_null(idx);
    assert_non_null(val);
    assert_non_null(dec);
    for (size_t i = 0; i < N_VALUES; i++) {
        idx[i] = (int32_t)(rng() % (NUM_DISTS + 1)) - 1;  /* 含 -1 与越界 */
        val[i] = (int32_t)(rng() % 401) - 200;            /* 大量 bypass 离群值 */
    }

    uint8_t *bits = NULL, *bits2 = NULL;
    size_t bits_sz = 0, bits2_sz = 0;
    assert_int_equal(rkvc_rans_encode(&coder, idx, val, N_VALUES, &bits, &bits_sz),
                     RKVC_RANS_OK);
    assert_non_null(bits);
    assert_true(bits_sz > 0);

    assert_int_equal(rkvc_rans_decode(&coder, dec, idx, N_VALUES, bits, bits_sz),
                     RKVC_RANS_OK);
    for (size_t i = 0; i < N_VALUES; i++) {
        /* index < 0 为契约性跳过：编码不写、解码回 0 */
        if (idx[i] >= 0)
            assert_int_equal(dec[i], val[i]);
        else
            assert_int_equal(dec[i], 0);
    }

    /* 确定性：重编码输出逐字节一致 */
    assert_int_equal(rkvc_rans_encode(&coder, idx, val, N_VALUES, &bits2, &bits2_sz),
                     RKVC_RANS_OK);
    assert_int_equal(bits2_sz, bits_sz);
    assert_memory_equal(bits, bits2, bits_sz);

    /* 截断码流必须报错 */
    assert_int_not_equal(rkvc_rans_decode(&coder, dec, idx, N_VALUES,
                                          bits, bits_sz / 2), RKVC_RANS_OK);

    free(bits);
    free(bits2);
    free(idx);
    free(val);
    free(dec);
    rkvc_rans_coder_free(&coder);
}

static void test_roundtrip_byte(void **state)
{
    roundtrip_variant(state, RKVC_RANS_BYTE);
}

static void test_roundtrip_64(void **state)
{
    roundtrip_variant(state, RKVC_RANS_64);
}

/* 流式多段编码（同流写多段，模拟 MLVC y1/y0/z 顺序）*/
static void streamed_variant(void **state, rkvc_rans_variant variant)
{
    (void)state;
    rng_state = 0xC0FFEEu;

    rkvc_rans_coder coder;
    assert_int_equal(rkvc_rans_coder_init(&coder, variant,
                                          pmf_lengths, NUM_DISTS,
                                          pmf_offsets, NUM_DISTS,
                                          pmf_table, TABLE_SIZE, 4, 2),
                     RKVC_RANS_OK);

    enum { M = 4096 };
    int32_t *idx = malloc(M * 3 * sizeof(int32_t));
    int32_t *val = malloc(M * 3 * sizeof(int32_t));
    int32_t *dec = malloc(M * 3 * sizeof(int32_t));
    assert_non_null(idx);
    assert_non_null(val);
    assert_non_null(dec);
    for (size_t i = 0; i < (size_t)M * 3; i++) {
        idx[i] = (int32_t)(rng() % NUM_DISTS);
        val[i] = (int32_t)(rng() % 81) - 40;
    }

    rkvc_rans_enc_stream es;
    rkvc_rans_enc_stream_init(&es, variant, 4096);
    assert_int_equal(rkvc_rans_enc_stream_encode(&es, &coder, idx + 2 * M, val + 2 * M, M),
                     RKVC_RANS_OK);
    assert_int_equal(rkvc_rans_enc_stream_encode(&es, &coder, idx + M, val + M, M),
                     RKVC_RANS_OK);
    assert_int_equal(rkvc_rans_enc_stream_encode(&es, &coder, idx, val, M),
                     RKVC_RANS_OK);
    size_t sz = 0;
    const uint8_t *bits = rkvc_rans_enc_stream_flush(&es, &sz);
    assert_non_null(bits);
    assert_true(sz > 0);

    rkvc_rans_dec_stream ds;
    rkvc_rans_dec_stream_init(&ds, variant);
    assert_int_equal(rkvc_rans_dec_stream_open(&ds, bits, sz), RKVC_RANS_OK);
    assert_int_equal(rkvc_rans_dec_stream_decode(&ds, &coder, dec, idx, M),
                     RKVC_RANS_OK);
    assert_int_equal(rkvc_rans_dec_stream_decode(&ds, &coder, dec + M, idx + M, M),
                     RKVC_RANS_OK);
    assert_int_equal(rkvc_rans_dec_stream_decode(&ds, &coder, dec + 2 * M, idx + 2 * M, M),
                     RKVC_RANS_OK);
    assert_true(rkvc_rans_dec_stream_check_eof(&ds));
    assert_memory_equal(val, dec, M * 3 * sizeof(int32_t));
    rkvc_rans_dec_stream_close(&ds);

    rkvc_rans_enc_stream_free(&es);
    free(idx);
    free(val);
    free(dec);
    rkvc_rans_coder_free(&coder);
}

static void test_streamed_byte(void **state)
{
    streamed_variant(state, RKVC_RANS_BYTE);
}

static void test_streamed_64(void **state)
{
    streamed_variant(state, RKVC_RANS_64);
}

static void test_invalid_pmf(void **state)
{
    (void)state;
    rkvc_rans_coder coder;
    /* 频率和超过 scale（16） */
    static const int32_t bad_table[] = { 8, 9 };
    static const int32_t lens[] = { 2 };
    static const int32_t offs[] = { 0 };
    assert_int_not_equal(rkvc_rans_coder_init(&coder, RKVC_RANS_BYTE, lens, 1,
                                              offs, 1, bad_table, 2, 4, 2),
                         RKVC_RANS_OK);
    /* 长度 ≤1 拒绝 */
    static const int32_t lens1[] = { 1 };
    assert_int_not_equal(rkvc_rans_coder_init(&coder, RKVC_RANS_BYTE, lens1, 1,
                                              offs, 1, bad_table, 1, 4, 2),
                         RKVC_RANS_OK);
    /* lengths/offsets 数量不一致 */
    assert_int_equal(rkvc_rans_coder_init(&coder, RKVC_RANS_BYTE, lens, 1,
                                          offs, 2, bad_table, 2, 4, 2),
                     RKVC_RANS_ERR_PARAMS);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roundtrip_byte),
        cmocka_unit_test(test_roundtrip_64),
        cmocka_unit_test(test_streamed_byte),
        cmocka_unit_test(test_streamed_64),
        cmocka_unit_test(test_invalid_pmf),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
