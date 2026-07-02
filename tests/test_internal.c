/**
 * @file test_internal.c
 * @brief v2 内部映射与 buffer 包装测试。
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include "internal.h"

static void test_averror_mapping(void **state)
{
    (void)state;
    assert_int_equal(rkvc_from_averror(AVERROR(ENOMEM)), RKVC_ERR_NOMEM);
    assert_int_equal(rkvc_from_averror(AVERROR(EAGAIN)), RKVC_ERR_AGAIN);
    assert_int_equal(rkvc_from_averror(AVERROR_EOF), RKVC_ERR_EOF);
}

static void test_pix_fmt_roundtrip(void **state)
{
    (void)state;
    assert_int_equal(rkvc_to_av_pix_fmt(RKVC_PIX_FMT_NV12), AV_PIX_FMT_NV12);
    assert_int_equal(rkvc_from_av_pix_fmt(AV_PIX_FMT_NV12), RKVC_PIX_FMT_NV12);
}

static void test_buffer_wrap_avframe(void **state)
{
    (void)state;
    AVFrame *av = av_frame_alloc();
    av->width = 64;
    av->height = 64;
    av->format = AV_PIX_FMT_NV12;
    assert_int_equal(rkvc_avframe_alloc_contiguous(av), RKVC_OK);

    rkvc_buffer *b = rkvc_buffer_wrap_avframe(av, 1);
    assert_non_null(b);
    rkvc_buffer_video_info info;
    assert_int_equal(rkvc_buffer_get_video_info(b, &info), RKVC_OK);
    assert_int_equal(info.width, 64u);
    rkvc_buffer_unref(b);
}

static void test_port_queue(void **state)
{
    (void)state;
    rkvc_port_queue *q = rkvc_port_queue_create(2);
    rkvc_buffer *b = NULL;
    rkvc_buffer_alloc_video_host(&b, 16, 16, RKVC_PIX_FMT_NV12);
    assert_int_equal(rkvc_port_queue_push(q, b), RKVC_OK);
    rkvc_buffer *out = NULL;
    assert_int_equal(rkvc_port_queue_pull(q, &out, 0), RKVC_OK);
    rkvc_buffer_unref(out);
    rkvc_buffer_unref(b);
    rkvc_port_queue_destroy(q);
}

static void test_hash_buffer(void **state)
{
    (void)state;
    const uint8_t data[] = "rkvc";
    char hex[65];

    assert_int_equal(rkvc_hash_buffer("sha256", data, sizeof(data) - 1,
                                      hex, sizeof(hex)), RKVC_OK);
    assert_string_equal(hex,
        "d14b3c138a0770f36abd90604e9c3672027d512c268fa4c6c44a001f9e2b2932");
}

static void test_dict_parse_opts(void **state)
{
    (void)state;
    AVDictionary *dict = NULL;

    assert_int_equal(rkvc_dict_parse_opts(&dict, "threads=1:low_delay=1"),
                     RKVC_OK);
    assert_non_null(dict);
    assert_non_null(av_dict_get(dict, "threads", NULL, 0));
    assert_non_null(av_dict_get(dict, "low_delay", NULL, 0));
    rkvc_dict_free(&dict);
}

static void test_now_us_monotonic(void **state)
{
    (void)state;
    int64_t t0 = rkvc_now_us();
    int64_t t1 = rkvc_now_us();
    assert_true(t1 >= t0);
}

int main(void)
{
    rkvc_init();

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_averror_mapping),
        cmocka_unit_test(test_pix_fmt_roundtrip),
        cmocka_unit_test(test_buffer_wrap_avframe),
        cmocka_unit_test(test_port_queue),
        cmocka_unit_test(test_hash_buffer),
        cmocka_unit_test(test_dict_parse_opts),
        cmocka_unit_test(test_now_us_monotonic),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
