/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_types.c
 * @brief v2 公共 API 单元测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

static void test_version_v2(void **state)
{
    (void)state;
    assert_non_null(rkvc_version());
    assert_true(rkvc_version_number() >= 0x00000200);
}

static void test_pipeline_defaults(void **state)
{
    (void)state;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    assert_int_equal(d.width, 1920);
    assert_int_equal(d.policy, RKVC_POLICY_BALANCED);
    assert_int_equal(d.codec, RKVC_CODEC_AUTO);
}

static void test_session_create_null(void **state)
{
    (void)state;
    rkvc_session *s = (rkvc_session *)(uintptr_t)1;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    assert_int_equal(rkvc_session_create(NULL, &s), RKVC_ERR_INVALID);
    assert_int_equal(rkvc_session_create(&d, NULL), RKVC_ERR_INVALID);
}

static void test_init_idempotent(void **state)
{
    (void)state;
    assert_int_equal(rkvc_init(), RKVC_OK);
    assert_int_equal(rkvc_init(), RKVC_OK);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_version_v2),
        cmocka_unit_test(test_pipeline_defaults),
        cmocka_unit_test(test_session_create_null),
        cmocka_unit_test(test_init_idempotent),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
