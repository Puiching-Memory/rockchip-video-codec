/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_post_upscale.c
 * @brief 解码后上采样节点单元测试（RGA 硬件）。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

static void test_upscale_algo_names(void **state)
{
    (void)state;
    rkvc_upscale_algo algo;

    assert_int_equal(rkvc_upscale_algo_from_name("bilinear", &algo), 0);
    assert_int_equal(algo, RKVC_UPSCALE_BILINEAR);
    assert_string_equal(rkvc_upscale_algo_name(algo), "bilinear");

    assert_int_equal(rkvc_upscale_algo_from_name("bicubic", &algo), 0);
    assert_int_equal(algo, RKVC_UPSCALE_BICUBIC);

    assert_int_equal(rkvc_upscale_algo_from_name("rkvc_sr", &algo), 0);
    assert_int_equal(algo, RKVC_UPSCALE_AI_SR);
    assert_string_equal(rkvc_upscale_algo_name(algo), "rkvc_sr");

    assert_int_equal(rkvc_upscale_algo_from_name("invalid", &algo), -1);
}

static void test_pipeline_post_upscale_defaults(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    assert_int_equal(d.enc_scale_denom, 1);
    assert_int_equal(d.post_upscale_algo, RKVC_UPSCALE_NONE);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_upscale_algo_names),
        cmocka_unit_test(test_pipeline_post_upscale_defaults),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
