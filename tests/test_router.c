/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_router.c
 * @brief Codec Router 单元测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

static void test_realtime_routes_h264(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy = RKVC_POLICY_REALTIME;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_H264);
    assert_int_equal(plan.enc_backend, RKVC_ENC_BACKEND_MPP);
}

static void test_balanced_routes_hevc(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy = RKVC_POLICY_BALANCED;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_HEVC);
}

static void test_quality_routes_av1(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy = RKVC_POLICY_QUALITY;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_AV1);
    assert_int_equal(plan.enc_backend, RKVC_ENC_BACKEND_SVT);
    assert_int_equal(plan.svt_preset, 11);
}

static void test_offline_routes_av1_hq(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy = RKVC_POLICY_OFFLINE;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_AV1);
    assert_int_equal(plan.enc_backend, RKVC_ENC_BACKEND_SVT);
    assert_int_equal(plan.svt_preset, 4);
    assert_string_equal(rkvc_policy_name(RKVC_POLICY_OFFLINE), "offline");
}

static void test_forced_codec(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.codec = RKVC_CODEC_H264;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_string_equal(plan.enc_name, "h264_rkmpp");
}

static void test_balanced_high_fps_1080p_downgrades_h264(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy  = RKVC_POLICY_BALANCED;
    d.width   = 1920;
    d.height  = 1080;
    d.fps_num = 60;
    d.fps_den = 1;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_H264);
    assert_string_equal(plan.enc_name, "h264_rkmpp");
}

static void test_neural_routes_mlvc(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.policy = RKVC_POLICY_NEURAL;
    rkvc_route_plan plan;
    assert_int_equal(rkvc_route_resolve(&d, &plan), RKVC_OK);
    assert_int_equal(plan.codec, RKVC_CODEC_MLVC);
    assert_int_equal(plan.enc_backend, RKVC_ENC_BACKEND_MLVC);
    assert_string_equal(rkvc_policy_name(RKVC_POLICY_NEURAL), "neural");
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_realtime_routes_h264),
        cmocka_unit_test(test_balanced_routes_hevc),
        cmocka_unit_test(test_quality_routes_av1),
        cmocka_unit_test(test_offline_routes_av1_hq),
        cmocka_unit_test(test_neural_routes_mlvc),
        cmocka_unit_test(test_forced_codec),
        cmocka_unit_test(test_balanced_high_fps_1080p_downgrades_h264),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
