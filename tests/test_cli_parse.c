/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_cli_parse.c
 * @brief CLI 共用参数解析。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "cli_parse.h"

static void test_parse_policy(void **state)
{
    (void)state;
    rkvc_policy p = RKVC_POLICY_REALTIME;
    assert_int_equal(rkvc_cli_parse_policy("balanced", &p), 0);
    assert_int_equal(p, RKVC_POLICY_BALANCED);
    assert_int_equal(rkvc_cli_parse_policy("neural", &p), 0);
    assert_int_equal(p, RKVC_POLICY_NEURAL);
    assert_int_equal(rkvc_cli_parse_policy("nope", &p), -1);
    assert_int_equal(rkvc_cli_parse_policy(NULL, &p), -1);
}

static void test_parse_wxh(void **state)
{
    (void)state;
    int w = 0, h = 0;
    assert_int_equal(rkvc_cli_parse_wxh("1920x1080", &w, &h), 0);
    assert_int_equal(w, 1920);
    assert_int_equal(h, 1080);
    assert_int_equal(rkvc_cli_parse_wxh("foobar", &w, &h), -1);
    assert_int_equal(rkvc_cli_parse_wxh("1920x1080foo", &w, &h), -1);
    assert_int_equal(rkvc_cli_parse_wxh("0x720", &w, &h), -1);
}

static void test_parse_rc_codec_pix(void **state)
{
    (void)state;
    rkvc_rc_mode rc = RKVC_RC_VBR;
    assert_int_equal(rkvc_cli_parse_rc_mode("cbr", &rc), 0);
    assert_int_equal(rc, RKVC_RC_CBR);
    assert_int_equal(rkvc_cli_parse_rc_mode("fixqp", &rc), 0);
    assert_int_equal(rc, RKVC_RC_CQP);
    assert_int_equal(rkvc_cli_parse_rc_mode("abr", &rc), -1);

    rkvc_codec c = RKVC_CODEC_AUTO;
    assert_int_equal(rkvc_cli_parse_codec("hevc", &c), 0);
    assert_int_equal(c, RKVC_CODEC_HEVC);
    assert_int_equal(rkvc_cli_parse_codec("mlvc", &c), 0);
    assert_int_equal(c, RKVC_CODEC_MLVC);
    assert_int_equal(rkvc_cli_parse_codec("vp9", &c), -1);

    rkvc_pix_fmt fmt = RKVC_PIX_FMT_NV12;
    assert_int_equal(rkvc_cli_parse_pix_fmt("yuv420p", &fmt), 0);
    assert_int_equal(fmt, RKVC_PIX_FMT_YUV420P);
    assert_int_equal(rkvc_cli_parse_pix_fmt("rgb", &fmt), -1);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_parse_policy),
        cmocka_unit_test(test_parse_wxh),
        cmocka_unit_test(test_parse_rc_codec_pix),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
