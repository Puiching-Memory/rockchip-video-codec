/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_fault_injection.c
 * @brief v2 确定性 OOM 测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <cmocka.h>
#include "internal.h"

#ifndef RKVC_ENABLE_FAULT_INJECTION
#error "test_fault_injection.c requires RKVC_ENABLE_FAULT_INJECTION"
#endif

static int clear_faults(void **state)
{
    (void)state;
    rkvc_test_clear_faults();
    return 0;
}

static void test_buffer_alloc_oom(void **state)
{
    (void)state;
    rkvc_buffer *b = (rkvc_buffer *)(uintptr_t)0x1;
    rkvc_test_fail_alloc_after(0);
    assert_int_equal(rkvc_buffer_alloc_video_host(&b, 64, 64,
                                                    RKVC_PIX_FMT_NV12),
                     RKVC_ERR_NOMEM);
    assert_null(b);

    rkvc_test_clear_faults();
    assert_int_equal(rkvc_buffer_alloc_video_host(&b, 64, 64,
                                                    RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    rkvc_buffer_unref(b);
}

static void test_session_create_oom(void **state)
{
    (void)state;
    rkvc_session *s = (rkvc_session *)(uintptr_t)0x1;
    rkvc_pipeline_desc d = rkvc_pipeline_desc_defaults();
    rkvc_test_fail_alloc_after(0);
    assert_int_equal(rkvc_session_create(&d, &s), RKVC_ERR_NOMEM);
    assert_null(s);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_buffer_alloc_oom,
                                        clear_faults, clear_faults),
        cmocka_unit_test_setup_teardown(test_session_create_oom,
                                        clear_faults, clear_faults),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
