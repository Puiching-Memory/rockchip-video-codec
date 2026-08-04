/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_v4l2.c
 * @brief V4L2 mock 采集单元测试（无需真实摄像头）。
 *
 * `capture_device="mock"` 走合成 NV12；可选硬件路径用
 * `RKVC_RUN_HARDWARE_TESTS=1` + 真实设备跑 LIVE_CAPTURE 短录。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <unistd.h>

#include "rkvc/rkvc.h"
#include "test_support.h"
#include "internal.h"

static void test_v4l2_mock_open_read(void **state)
{
    (void)state;
    rkvc_v4l2_config cfg = {
        .device = "mock",
        .width = 320,
        .height = 240,
        .fps_num = 30,
        .fps_den = 1,
    };
    rkvc_v4l2_cap *cap = NULL;
    assert_int_equal(rkvc_v4l2_open(&cap, &cfg), RKVC_OK);
    assert_non_null(cap);

    int w = 0, h = 0;
    rkvc_v4l2_get_size(cap, &w, &h);
    assert_int_equal(w, 320);
    assert_int_equal(h, 240);

    for (int i = 0; i < 5; i++) {
        rkvc_buffer *frame = NULL;
        assert_int_equal(rkvc_v4l2_read_frame(cap, &frame, 0), RKVC_OK);
        assert_non_null(frame);
        assert_int_equal(frame->kind, RKVC_BUF_VIDEO);
        assert_int_equal(frame->format, RKVC_PIX_FMT_NV12);
        assert_int_equal((int)frame->width, 320);
        assert_int_equal((int)frame->height, 240);
        assert_int_equal(frame->pts, i);
        rkvc_buffer_unref(frame);
    }

    rkvc_v4l2_close(cap);
}

static void test_v4l2_mock_invalid(void **state)
{
    (void)state;
    rkvc_v4l2_config cfg = {
        .device = "mock",
        .width = 0,
        .height = 240,
    };
    rkvc_v4l2_cap *cap = NULL;
    assert_int_equal(rkvc_v4l2_open(&cap, &cfg), RKVC_ERR_INVALID);
    assert_null(cap);

    cfg.width = 320;
    cfg.device = NULL;
    assert_int_equal(rkvc_v4l2_open(&cap, &cfg), RKVC_ERR_INVALID);
}

static void test_live_capture_mock_session(void **state)
{
    (void)state;
    if (!rkvc_test_hardware_opted_in()) {
        skip();
        return;
    }

    rkvc_pipeline_desc d;
    assert_int_equal(
        rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d), RKVC_OK);
    d.capture_device = "mock";
    d.output_path = "/tmp/rkvc_v4l2_mock.mp4";
    d.width = 320;
    d.height = 240;
    d.capture_max_frames = 8;
    d.bitrate = 500000;
    d.gop_size = 8;

    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_OK);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    rkvc_session_destroy(s);

    assert_int_equal(err, RKVC_OK);
    assert_true(st.frames_in >= 8);
    assert_true(st.frames_out > 0);
    assert_int_equal(access("/tmp/rkvc_v4l2_mock.mp4", R_OK), 0);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_v4l2_mock_open_read),
        cmocka_unit_test(test_v4l2_mock_invalid),
        cmocka_unit_test(test_live_capture_mock_session),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
