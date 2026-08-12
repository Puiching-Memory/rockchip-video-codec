/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_scale.c
 * @brief RGA 缩放节点测试 (buffer API)。
 *
 * 布局测试始终运行；硬件用例需 RKVC_RUN_HARDWARE_TESTS=1。
 * 推广门禁：./scripts/test-rga.sh
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cmocka.h>
#include "internal.h"
#include "test_support.h"

static void skip_if_no_rga(void)
{
    if (!rkvc_test_hardware_opted_in())
        skip();
    if (!rkvc_rga_available())
        skip();
}

static void fill_nv12(rkvc_buffer *b, int seed)
{
    uint8_t *planes[4] = {0};
    int strides[4] = {0};
    rkvc_buffer_get_video_planes(b, planes, strides);
    rkvc_buffer_video_info info;
    rkvc_buffer_get_video_info(b, &info);

    for (uint32_t y = 0; y < info.height; y++) {
        for (uint32_t x = 0; x < info.width; x++)
            planes[0][y * strides[0] + x] =
                (uint8_t)((x * 3u + y * 5u + (uint32_t)seed) & 0xffu);
    }

    const uint32_t ch = info.height / 2u;
    const uint32_t cw = info.width / 2u;
    for (uint32_t y = 0; y < ch; y++) {
        uint8_t *uv = planes[1] + y * strides[1];
        for (uint32_t x = 0; x < cw; x++) {
            uv[2 * x]     = (uint8_t)((x * 7u + y * 3u + seed) & 0xffu);
            uv[2 * x + 1] = (uint8_t)((x * 5u + y * 11u + seed) & 0xffu);
        }
    }
}

static void fill_nv12_frame(AVFrame *f, int seed)
{
    for (int y = 0; y < f->height; y++) {
        for (int x = 0; x < f->width; x++)
            f->data[0][y * f->linesize[0] + x] =
                (uint8_t)((x * 3 + y * 5 + seed) & 0xff);
    }
    const int ch = f->height / 2;
    const int cw = f->width / 2;
    for (int y = 0; y < ch; y++) {
        uint8_t *uv = f->data[1] + y * f->linesize[1];
        for (int x = 0; x < cw; x++) {
            uv[2 * x]     = (uint8_t)((x * 7 + y * 3 + seed) & 0xff);
            uv[2 * x + 1] = (uint8_t)((x * 5 + y * 11 + seed) & 0xff);
        }
    }
}

static rkvc_err scale_nv12(const rkvc_buffer *src, rkvc_buffer **dst,
                           int dw, int dh, rkvc_upscale_algo algo)
{
    return rkvc_rga_scale_buffer(src, dst, dw, dh, RKVC_PIX_FMT_NV12, algo);
}

static void assert_scale_ok(const rkvc_buffer *src, rkvc_buffer **dst,
                          int dw, int dh, rkvc_upscale_algo algo)
{
    assert_int_equal(scale_nv12(src, dst, dw, dh, algo), RKVC_OK);
    rkvc_buffer_video_info info;
    rkvc_buffer_get_video_info(*dst, &info);
    assert_int_equal((int)info.width, dw);
    assert_int_equal((int)info.height, dh);
}

static int bottom_uv_all_zero(const rkvc_buffer *b, int rows)
{
    uint8_t *planes[4] = {0};
    int strides[4] = {0};
    rkvc_buffer_get_video_planes((rkvc_buffer *)b, planes, strides);
    rkvc_buffer_video_info info;
    rkvc_buffer_get_video_info(b, &info);

    if (rows > (int)info.height / 2)
        rows = (int)info.height / 2;

    const int y0 = (int)info.height / 2 - rows;
    for (int y = y0; y < (int)info.height / 2; y++) {
        const uint8_t *uv = planes[1] + y * strides[1];
        for (uint32_t x = 0; x < info.width; x += 2) {
            if (uv[x] != 0 || uv[x + 1] != 0)
                return 0;
        }
    }
    return 1;
}

static rkvc_buffer *alloc_padded_nv12_1080p(int seed)
{
    AVFrame *f = av_frame_alloc();
    if (!f)
        return NULL;

    f->format = AV_PIX_FMT_NV12;
    f->width  = 1920;
    f->height = 1080;
    if (av_frame_get_buffer(f, 32) < 0) {
        av_frame_free(&f);
        return NULL;
    }

    fill_nv12_frame(f, seed);
    rkvc_buffer *b = rkvc_buffer_wrap_avframe(f, 1);
    if (b)
        b->format = RKVC_PIX_FMT_NV12;
    return b;
}

static void test_contiguous_alloc(void **state)
{
    (void)state;
    rkvc_buffer *b = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&b, 1920, 1080,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    uint8_t *planes[4] = {0};
    int strides[4] = {0};
    rkvc_buffer_get_video_planes(b, planes, strides);
    assert_ptr_equal(planes[1], planes[0] + (ptrdiff_t)strides[0] * 1080);
    rkvc_buffer_unref(b);
}

static void test_rga_downscale_vga(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 640, 480,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 1);
    assert_scale_ok(src, &dst, 320, 240, RKVC_UPSCALE_BILINEAR);
    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

static void test_rga_downscale_1080p_to_360p(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 1920, 1080,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 11);
    assert_scale_ok(src, &dst, 640, 360, RKVC_UPSCALE_BILINEAR);
    assert_int_equal(bottom_uv_all_zero(dst, 8), 0);
    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

static void test_rga_upscale_360p_to_1080p(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 640, 360,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 22);

    assert_scale_ok(src, &dst, 1920, 1080, RKVC_UPSCALE_NEAREST);
    rkvc_buffer_unref(dst);

    assert_scale_ok(src, &dst, 1920, 1080, RKVC_UPSCALE_BILINEAR);
    assert_int_equal(bottom_uv_all_zero(dst, 16), 0);
    rkvc_buffer_unref(dst);

    assert_scale_ok(src, &dst, 1920, 1080, RKVC_UPSCALE_BICUBIC);
    assert_int_equal(bottom_uv_all_zero(dst, 16), 0);
    rkvc_buffer_unref(dst);

    rkvc_buffer_unref(src);
}

static void test_rga_bench_roundtrip(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *mid = NULL, *out = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 1920, 1080,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 33);
    assert_scale_ok(src, &mid, 640, 360, RKVC_UPSCALE_BILINEAR);
    assert_scale_ok(mid, &out, 1920, 1080, RKVC_UPSCALE_BILINEAR);
    assert_int_equal(bottom_uv_all_zero(out, 16), 0);

    uint8_t *sp[4] = {0}, *op[4] = {0};
    int ss[4] = {0}, os[4] = {0};
    rkvc_buffer_get_video_planes(src, sp, ss);
    rkvc_buffer_get_video_planes(out, op, os);
    assert_int_not_equal(sp[0][0], op[0][0]);

    rkvc_buffer_unref(out);
    rkvc_buffer_unref(mid);
    rkvc_buffer_unref(src);
}

static void test_rga_padded_source_upscale(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = alloc_padded_nv12_1080p(44);
    assert_non_null(src);

    rkvc_buffer *dst = NULL;
    assert_scale_ok(src, &dst, 640, 360, RKVC_UPSCALE_BILINEAR);
    assert_int_equal(bottom_uv_all_zero(dst, 8), 0);

    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

static void test_post_upscale_buffer_bench(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 640, 360,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 55);
    assert_int_equal(rkvc_post_upscale_buffer(src, &dst, 1920, 1080,
                                            RKVC_UPSCALE_BILINEAR, NULL),
                     RKVC_OK);
    rkvc_buffer_video_info info;
    rkvc_buffer_get_video_info(dst, &info);
    assert_int_equal((int)info.width, 1920);
    assert_int_equal((int)info.height, 1080);
    assert_int_equal(bottom_uv_all_zero(dst, 16), 0);
    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

static void test_rga_same_size_short_circuit(void **state)
{
    (void)state;
    skip_if_no_rga();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 1920, 1080,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 66);
    assert_int_equal(scale_nv12(src, &dst, 1920, 1080, RKVC_UPSCALE_BILINEAR),
                     RKVC_OK);
    assert_ptr_equal(dst, src);
    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

static void test_rga_soak_frames(void **state)
{
    (void)state;
    skip_if_no_rga();

    const char *env = getenv("RKVC_RGA_SOAK_FRAMES");
    if (!env || env[0] == '\0' || env[0] == '0')
        skip();

    int frames = atoi(env);
    if (frames <= 0)
        skip();

    rkvc_buffer *src = NULL, *dst = NULL;
    assert_int_equal(rkvc_buffer_alloc_video_host(&src, 640, 360,
                                                  RKVC_PIX_FMT_NV12),
                     RKVC_OK);
    fill_nv12(src, 77);

    for (int i = 0; i < frames; i++) {
        rkvc_buffer_unref(dst);
        dst = NULL;
        assert_int_equal(scale_nv12(src, &dst, 1920, 1080,
                                    RKVC_UPSCALE_BILINEAR),
                         RKVC_OK);
    }

    rkvc_buffer_unref(dst);
    rkvc_buffer_unref(src);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_contiguous_alloc),
        cmocka_unit_test(test_rga_downscale_vga),
        cmocka_unit_test(test_rga_downscale_1080p_to_360p),
        cmocka_unit_test(test_rga_upscale_360p_to_1080p),
        cmocka_unit_test(test_rga_bench_roundtrip),
        cmocka_unit_test(test_rga_padded_source_upscale),
        cmocka_unit_test(test_post_upscale_buffer_bench),
        cmocka_unit_test(test_rga_same_size_short_circuit),
        cmocka_unit_test(test_rga_soak_frames),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
