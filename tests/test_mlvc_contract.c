/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * Python 工具 ↔ C 库二进制契约回归（PMF1 + QPP1）。
 *
 * fixture 由 tests/gen_mlvc_fixtures.py 用 tools/mlvc/pmf.py / qppatch.py 生成，
 * 本测试用 C 消费方代码（rkvc_pmf_load / rkvc_rans_coder_init /
 * rkvc_qppatch_apply）加载并核对——任一侧对格式的理解不一致都会在此爆红。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmf.h"
#include "rans.h"
#include "qppatch.h"

static const char *fixture_path(const char *name, char *buf, size_t cap)
{
    const char *root = getenv("RKVC_SOURCE_ROOT");
    if (!root) {
        fail_msg("RKVC_SOURCE_ROOT 未设置（应由 CTest ENVIRONMENT 提供）");
        return NULL;
    }
    snprintf(buf, cap, "%s/tests/fixtures/mlvc/%s", root, name);
    return buf;
}

static unsigned char *read_file(const char *path, size_t *out_sz)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fail_msg("无法打开 %s", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? (size_t)n : 1);
    if (!buf) {
        fclose(f);
        fail_msg("OOM");
        return NULL;
    }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        fail_msg("读取 %s 失败", path);
        return NULL;
    }
    fclose(f);
    *out_sz = (size_t)n;
    return buf;
}

/* gaussian PMF1（Python 生成）→ C 逐字段核对 + rANS 可编解码 */
static void test_gaussian_pmf_contract(void **state)
{
    (void)state;
    char path[512];
    rkvc_pmf p;
    assert_int_equal(rkvc_pmf_load(&p,
                                   fixture_path("gaussian.bin", path, sizeof(path))),
                     RKVC_OK);

    assert_int_equal(p.num_lengths, 2);
    assert_int_equal(p.num_offsets, 2);
    assert_int_equal(p.num_table, 5);
    assert_int_equal(p.lengths[0], 2);
    assert_int_equal(p.lengths[1], 3);
    assert_int_equal(p.offsets[0], 0);
    assert_int_equal(p.offsets[1], 2);
    const int32_t expect_tab[5] = { 1, 2, 3, 4, 5 };
    assert_memory_equal(p.table, expect_tab, sizeof(expect_tab));
    assert_true(p.scale_min > 0.109 && p.scale_min < 0.111);
    assert_true(p.scale_max > 15.9 && p.scale_max < 16.1);
    assert_int_equal(p.scale_levels, 128);
    assert_int_equal(p.index_space, 1);

    /* Python 生成的表 → C 熵编码器可初始化并往返 */
    rkvc_rans_coder coder;
    assert_int_equal(rkvc_rans_coder_init(&coder, RKVC_RANS_BYTE,
                                          p.lengths, p.num_lengths,
                                          p.offsets, p.num_offsets,
                                          p.table, p.num_table, 16, 2),
                     RKVC_RANS_OK);

    enum { N = 64, MAXD = 2 };
    int32_t idx[N], val[N], dec[N];
    for (int i = 0; i < N; i++) {
        idx[i] = i % MAXD;
        /* 分布0 值域 [0,1]；分布1 值域 [2,4] */
        val[i] = (i % MAXD == 0) ? (int32_t)(i % 2) : (int32_t)(2 + i % 3);
    }
    uint8_t *bits = NULL;
    size_t sz = 0;
    assert_int_equal(rkvc_rans_encode(&coder, idx, val, N, &bits, &sz), RKVC_RANS_OK);
    assert_non_null(bits);
    assert_true(sz > 0);
    assert_int_equal(rkvc_rans_decode(&coder, dec, idx, N, bits, sz), RKVC_RANS_OK);
    assert_memory_equal(val, dec, sizeof(val));

    free(bits);
    rkvc_rans_coder_free(&coder);
    rkvc_pmf_free(&p);
}

/* bitest PMF1（Python 生成）→ C 逐字段核对 + 熵编码器可初始化 */
static void test_bitest_pmf_contract(void **state)
{
    (void)state;
    char path[512];
    rkvc_pmf p;
    assert_int_equal(rkvc_pmf_load(&p,
                                   fixture_path("bitest.bin", path, sizeof(path))),
                     RKVC_OK);

    assert_int_equal(p.num_lengths, 4);
    assert_int_equal(p.num_offsets, 4);
    assert_int_equal(p.num_table, 22);
    const int32_t expect_lens[4] = { 4, 5, 6, 7 };
    const int32_t expect_offs[4] = { 0, 4, 9, 15 };
    assert_memory_equal(p.lengths, expect_lens, sizeof(expect_lens));
    assert_memory_equal(p.offsets, expect_offs, sizeof(expect_offs));
    assert_int_equal(p.qp_num, 2);
    assert_int_equal(p.channels, 2);

    rkvc_rans_coder coder;
    assert_int_equal(rkvc_rans_coder_init(&coder, RKVC_RANS_BYTE,
                                          p.lengths, p.num_lengths,
                                          p.offsets, p.num_offsets,
                                          p.table, p.num_table, 16, 2),
                     RKVC_RANS_OK);
    rkvc_rans_coder_free(&coder);
    rkvc_pmf_free(&p);
}

/* QPP1（Python 生成）→ C 应用后与 Python 侧目标逐字节一致 */
static void test_qppatch_contract(void **state)
{
    (void)state;
    char path[512];
    size_t base_sz = 0, target_sz = 0, patch_sz = 0;
    unsigned char *base = read_file(fixture_path("qppatch_base.bin", path, sizeof(path)),
                                    &base_sz);
    assert_non_null(base);
    assert_int_equal(base_sz, 4096);
    unsigned char *target = read_file(
        fixture_path("qppatch_target.bin", path, sizeof(path)), &target_sz);
    assert_non_null(target);
    assert_int_equal(target_sz, base_sz);
    unsigned char *patch = read_file(fixture_path("qppatch_qp1.bin", path, sizeof(path)),
                                     &patch_sz);
    assert_non_null(patch);
    assert_true(patch_sz > 0);

    assert_int_equal(rkvc_qppatch_apply(base, base_sz, patch, patch_sz, 1), RKVC_OK);
    assert_memory_equal(base, target, base_sz);

    /* 预期 qp 不符必须拒绝 */
    unsigned char *base2 = read_file(fixture_path("qppatch_base.bin", path, sizeof(path)),
                                     &base_sz);
    assert_non_null(base2);
    assert_int_not_equal(rkvc_qppatch_apply(base2, base_sz, patch, patch_sz, 2), RKVC_OK);

    free(base);
    free(base2);
    free(target);
    free(patch);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_gaussian_pmf_contract),
        cmocka_unit_test(test_bitest_pmf_contract),
        cmocka_unit_test(test_qppatch_contract),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
