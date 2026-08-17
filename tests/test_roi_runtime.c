/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_roi_runtime.c
 * @brief ROI / runtime 配额单元测试（无需硬件）。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"
#include "internal.h"

static void test_roi_set_clear(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.template_id = RKVC_TEMPLATE_FILE_ENCODE;
    d.output_path = "/tmp/rkvc_roi_dummy.mp4";
    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_OK);
    assert_non_null(s);

    rkvc_roi_rect r = { .x = 10, .y = 10, .w = 100, .h = 80,
                        .qp_offset = -3, .force_intra = 1 };
    assert_int_equal(rkvc_session_set_roi(s, &r, 1), RKVC_OK);
    rkvc_roi_rect neg = { .x = -1, .y = 0, .w = 10, .h = 10 };
    assert_int_equal(rkvc_session_set_roi(s, &neg, 1), RKVC_ERR_INVALID);
    rkvc_roi_rect oob = { .x = 1900, .y = 10, .w = 100, .h = 80 };
    assert_int_equal(rkvc_session_set_roi(s, &oob, 1), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_session_set_roi(s, &r, RKVC_ROI_MAX + 1),
                     RKVC_ERR_INVALID);
    assert_int_equal(rkvc_session_clear_roi(s), RKVC_OK);

    rkvc_session_destroy(s);
}

static void test_runtime_quota(void **state)
{
    (void)state;
    rkvc_runtime_quota q = { .max_sessions = 1, .max_enc_sessions = 1,
                             .max_npu_sessions = 0 };
    assert_int_equal(rkvc_runtime_set_quota(&q), RKVC_OK);

    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.template_id = RKVC_TEMPLATE_FILE_ENCODE;
    d.output_path = "/tmp/a.mp4";
    rkvc_session *s1 = NULL;
    rkvc_session *s2 = NULL;
    assert_int_equal(rkvc_session_create(&d, &s1), RKVC_OK);
    assert_int_equal(rkvc_session_create(&d, &s2), RKVC_ERR_AGAIN);
    assert_null(s2);

    rkvc_runtime_stats st;
    assert_int_equal(rkvc_runtime_get_stats(&st), RKVC_OK);
    assert_int_equal(st.sessions, 1);
    assert_int_equal(st.enc_sessions, 1);

    rkvc_session_destroy(s1);
    assert_int_equal(rkvc_runtime_get_stats(&st), RKVC_OK);
    assert_int_equal(st.sessions, 0);

    assert_int_equal(rkvc_runtime_set_quota(NULL), RKVC_OK);
}

static void test_runtime_quota_mlvc(void **state)
{
    (void)state;
    rkvc_runtime_quota q = { .max_sessions = 2, .max_enc_sessions = 1,
                             .max_npu_sessions = 1 };
    assert_int_equal(rkvc_runtime_set_quota(&q), RKVC_OK);

    rkvc_pipeline_desc d;
    assert_int_equal(
        rkvc_pipeline_from_template(RKVC_TEMPLATE_MLVC_STORAGE, &d), RKVC_OK);
    d.output_path = "/tmp/rkvc_quota.mlvc";

    rkvc_session *s1 = NULL;
    rkvc_session *s2 = NULL;
    assert_int_equal(rkvc_session_create(&d, &s1), RKVC_OK);
    assert_non_null(s1);

    rkvc_runtime_stats st;
    assert_int_equal(rkvc_runtime_get_stats(&st), RKVC_OK);
    assert_int_equal(st.enc_sessions, 1);
    assert_int_equal(st.npu_sessions, 1);

    assert_int_equal(rkvc_session_create(&d, &s2), RKVC_ERR_AGAIN);
    assert_null(s2);

    rkvc_session_destroy(s1);
    assert_int_equal(rkvc_runtime_set_quota(NULL), RKVC_OK);
}

static void test_live_template_defaults(void **state)
{
    (void)state;
    rkvc_pipeline_desc d;
    assert_int_equal(
        rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d), RKVC_OK);
    assert_int_equal(d.policy, RKVC_POLICY_REALTIME);
    assert_int_equal(d.low_latency, 1);
    assert_int_equal(d.capture_timeout_ms, 1000);
}

static void test_start_retry_after_invalid_live(void **state)
{
    (void)state;
    rkvc_pipeline_desc d;
    assert_int_equal(
        rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d), RKVC_OK);
    d.output_path = "/tmp/rkvc_start_retry.mp4";
    d.width = 320;
    d.height = 240;

    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_OK);
    assert_int_equal(rkvc_session_start(s), RKVC_ERR_INVALID);

    s->desc.capture_device = "mock";
    s->desc.capture_max_frames = 1;
    rkvc_err err = rkvc_session_start(s);
    if (err == RKVC_OK)
        rkvc_session_stop(s);
    rkvc_session_destroy(s);
}

static void test_enc_scale_rejects_zero(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    d.template_id = RKVC_TEMPLATE_FILE_ENCODE;
    d.output_path = "/tmp/rkvc_scale_dummy.mp4";
    d.width = 16;
    d.height = 16;
    d.enc_scale_denom = 32;
    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_ERR_INVALID);
    assert_null(s);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_roi_set_clear),
        cmocka_unit_test(test_enc_scale_rejects_zero),
        cmocka_unit_test(test_runtime_quota),
        cmocka_unit_test(test_runtime_quota_mlvc),
        cmocka_unit_test(test_live_template_defaults),
        cmocka_unit_test(test_start_retry_after_invalid_live),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
