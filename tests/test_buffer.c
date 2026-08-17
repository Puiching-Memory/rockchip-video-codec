/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_buffer.c
 * @brief rkvc_buffer 单元测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "rkvc/rkvc.h"

#include <stdint.h>

static void test_alloc_video_host(void **state)
{
    (void)state;
    rkvc_buffer *b = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&b, 64, 64,
                                                    RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    assert_non_null(b);
    assert_int_equal(rkvc_buffer_kind_of(b), RKVC_BUF_VIDEO);

    rkvc_buffer_video_info info;
    assert_int_equal(rkvc_buffer_get_video_info(b, &info), RKVC_OK);
    assert_int_equal(info.width, 64u);
    assert_int_equal(info.mem_type, RKVC_MEM_HOST);

    rkvc_buffer_unref(b);
}

static void test_bitstream_buffer(void **state)
{
    (void)state;
    const uint8_t data[] = {0, 0, 0, 1, 0x40};
    rkvc_buffer *b = NULL;
    assert_int_equal(rkvc_buffer_alloc_bitstream(&b, data, sizeof(data), 1),
                     RKVC_OK);
    rkvc_buffer_bitstream_view view;
    assert_int_equal(rkvc_buffer_get_bitstream(b, &view), RKVC_OK);
    assert_int_equal(view.size, sizeof(data));
    rkvc_buffer_unref(b);
}

static void test_bitstream_rejects_overflowing_size(void **state)
{
    (void)state;
    const uint8_t byte = 0;
    rkvc_buffer *buffer = NULL;
    assert_int_equal(rkvc_buffer_alloc_bitstream(&buffer, &byte, SIZE_MAX, 1),
                     RKVC_ERR_INVALID);
    assert_null(buffer);
}

static void test_refcount(void **state)
{
    (void)state;
    rkvc_buffer *b = NULL;
    rkvc_buffer_alloc_video_host(&b, 16, 16, RKVC_PIX_FMT_NV12);
    rkvc_buffer *r = rkvc_buffer_ref(b);
    rkvc_buffer_unref(b);
    rkvc_buffer_unref(r);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_alloc_video_host),
        cmocka_unit_test(test_bitstream_buffer),
        cmocka_unit_test(test_bitstream_rejects_overflowing_size),
        cmocka_unit_test(test_refcount),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
