/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_contracts.c
 * @brief Session / Port 契约测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

static void test_query_caps_contracts(void **state)
{
    (void)state;
    rkvc_caps caps;
    assert_int_equal(rkvc_query_caps(NULL), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_query_caps(&caps), RKVC_OK);
    assert_true(caps.max_width >= 1920);
    assert_true(caps.has_av1_enc == 1);
    assert_true(caps.has_rknn == 0 || caps.has_rknn == 1);
}

static void test_port_null(void **state)
{
    (void)state;
    rkvc_buffer *b = NULL;
    assert_int_equal(rkvc_port_push(NULL, NULL), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_port_pull(NULL, &b, 0), RKVC_ERR_INVALID);
}

static void test_session_port_names(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    rkvc_session *s = NULL;
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_OK);
    assert_non_null(rkvc_session_port(s, "capture"));
    assert_non_null(rkvc_session_port(s, "output"));
    assert_null(rkvc_session_port(s, "nope"));
    rkvc_session_destroy(s);
}

static void test_pipeline_template(void **state)
{
    (void)state;
    rkvc_pipeline_desc d;
    assert_int_equal(rkvc_pipeline_from_template(RKVC_TEMPLATE_AV1_STORAGE, &d),
                     RKVC_OK);
    assert_int_equal(d.codec, RKVC_CODEC_AV1);
    assert_int_equal(d.policy, RKVC_POLICY_QUALITY);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_query_caps_contracts),
        cmocka_unit_test(test_port_null),
        cmocka_unit_test(test_session_port_names),
        cmocka_unit_test(test_pipeline_template),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
