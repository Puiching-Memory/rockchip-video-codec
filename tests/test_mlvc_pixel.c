/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * MLVC 像素内核回归：NEON 快速路径与优化前标量参考实现逐位等价。
 * 参考实现逐行复制自优化前的 node_mlvc.c 内联函数。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "mlvc_pixel.h"

static uint32_t rng_state;

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* ═══ 参考实现（优化前） ═══ */

static void ref_extract_scales(const int32_t *z_r, int32_t *s0, int32_t *s1,
                               int YC, int YH, int YW, int ZH, int ZW)
{
    for (int c = 0; c < YC; c++) {
        int zc0 = c / 4;
        int zc1 = (c + YC) / 4;
        for (int y = 0; y < YH; y++) {
            int zy = y / 4;
            for (int x = 0; x < YW; x++) {
                int zx = x / 4;
                int v0 = abs(z_r[(zc0 * ZH + zy) * ZW + zx]);
                int v1 = abs(z_r[(zc1 * ZH + zy) * ZW + zx]);
                if (v0 > 127) v0 = 127;
                if (v1 > 127) v1 = 127;
                int chk = ((y & 1) == (x & 1));
                size_t o = ((size_t)c * YH + y) * YW + x;
                s0[o] = chk ? v0 : v1;
                s1[o] = chk ? v1 : v0;
            }
        }
    }
}

static void ref_nc1hwc2_to_nchw(const uint16_t *src, int32_t *dst, int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] =
                    lrintf(mlvc_px_f16_to_f32(src[(((size_t)c1 * H + y) * W + x) * 8 + c2]));
    }
}

static void ref_nchw_to_nc1hwc2_fp16(const int32_t *src, uint16_t *dst, int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(((size_t)c1 * H + y) * W + x) * 8 + c2] =
                    mlvc_px_f32_to_f16((float)src[((size_t)c * H + y) * W + x]);
    }
}

static void ref_nc1hwc2_to_nchw_f16(const uint16_t *src, uint16_t *dst,
                                    int C1, int H, int W, int C2)
{
    int C = C1 * C2;
    for (int c = 0; c < C; c++) {
        int c1 = c / C2, c2 = c % C2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] =
                    src[(((size_t)c1 * H + y) * W + x) * C2 + c2];
    }
}

static void ref_depth_to_space_nchw_dcr_f16(const uint16_t *in, uint16_t *out,
                                            int C, int H, int W, int bs)
{
    int oc = C / (bs * bs);
    int oh = H * bs, ow = W * bs;
    for (int c = 0; c < oc; c++)
        for (int dy = 0; dy < bs; dy++)
            for (int dx = 0; dx < bs; dx++) {
                int ic = dy * (bs * oc) + dx * oc + c;
                for (int h = 0; h < H; h++)
                    for (int w = 0; w < W; w++)
                        out[(c * oh + h * bs + dy) * (size_t)ow + (w * bs + dx)] =
                            in[(ic * H + h) * (size_t)W + w];
            }
}

static void ref_clip01_f16(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float f = mlvc_px_f16_to_f32(p[i]);
        if (f <= 0.0f)
            p[i] = mlvc_px_f32_to_f16(0.0f);
        else if (f >= 1.0f)
            p[i] = mlvc_px_f32_to_f16(1.0f);
    }
}

static void ref_yuv_to_nhwc_fp16(const uint8_t *Yp, int y_stride,
                                 const uint8_t *Up, const uint8_t *Vp,
                                 int uv_stride, int is_nv12,
                                 int W, int H, uint16_t *nhwc)
{
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            size_t o = (size_t)y * W + x;
            int uy = y / 2, ux = x / 2;
            nhwc[o * 3 + 0] = mlvc_px_f32_to_f16(Yp[y * y_stride + x] * (1.0f / 255.0f));
            if (is_nv12) {
                nhwc[o * 3 + 1] = mlvc_px_f32_to_f16(Up[uy * uv_stride + ux * 2] * (1.0f / 255.0f));
                nhwc[o * 3 + 2] = mlvc_px_f32_to_f16(Vp[uy * uv_stride + ux * 2] * (1.0f / 255.0f));
            } else {
                nhwc[o * 3 + 1] = mlvc_px_f32_to_f16(Up[uy * uv_stride + ux] * (1.0f / 255.0f));
                nhwc[o * 3 + 2] = mlvc_px_f32_to_f16(Vp[uy * uv_stride + ux] * (1.0f / 255.0f));
            }
        }
}

static void ref_nchw_yuv_to_nv12(const uint16_t *src, int W, int H,
                                 uint8_t *Yp, int y_stride,
                                 uint8_t *UV, int uv_stride)
{
    size_t plane = (size_t)H * W;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int v = (int)lrintf(mlvc_px_f16_to_f32(src[(size_t)y * W + x]) * 255.0f);
            Yp[y * y_stride + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    for (int y = 0; y < H / 2; y++)
        for (int x = 0; x < W / 2; x++) {
            size_t o = (size_t)(y * 2) * W + (x * 2);
            int u = (int)lrintf(mlvc_px_f16_to_f32(src[plane + o]) * 255.0f);
            int v = (int)lrintf(mlvc_px_f16_to_f32(src[2 * plane + o]) * 255.0f);
            UV[y * uv_stride + x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            UV[y * uv_stride + x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
}

/* ═══ 测试 ═══ */

static void test_extract_scales(void **state)
{
    (void)state;
    rng_state = 0x1234567u;
    /* 组1：MLVC 640x368 实际几何；组2：非 4 倍数边界 */
    static const int cases[][5] = {
        { 16, 92, 160, 23, 40 },
        { 8,  19, 25,  5,  7 },   /* YH=4*5-1, YW=4*7-3 */
        { 4,  4,  4,   1,  1 },
    };
    for (size_t t = 0; t < 3; t++) {
        int YC = cases[t][0], YH = cases[t][1], YW = cases[t][2];
        int ZH = cases[t][3], ZW = cases[t][4];
        size_t zn = (size_t)(YC / 4 * 2) * ZH * ZW;
        size_t yn = (size_t)YC * YH * YW;
        int32_t *z = malloc(zn * sizeof(int32_t));
        int32_t *a0 = malloc(yn * sizeof(int32_t));
        int32_t *a1 = malloc(yn * sizeof(int32_t));
        int32_t *b0 = malloc(yn * sizeof(int32_t));
        int32_t *b1 = malloc(yn * sizeof(int32_t));
        assert_non_null(z);
        for (size_t i = 0; i < zn; i++)
            z[i] = (int32_t)(rng() % 601) - 300;
        ref_extract_scales(z, a0, a1, YC, YH, YW, ZH, ZW);
        mlvc_px_extract_scales(z, b0, b1, YC, YH, YW, ZH, ZW);
        assert_memory_equal(a0, b0, yn * sizeof(int32_t));
        assert_memory_equal(a1, b1, yn * sizeof(int32_t));
        free(z); free(a0); free(a1); free(b0); free(b1);
    }
}

static uint16_t rand_f16_finite(void)
{
    float f = ((float)(rng() % 20001) - 10000.0f) / 100.0f;  /* [-100, 100] */
    return mlvc_px_f32_to_f16(f);
}

static void test_nc1hwc2_to_nchw(void **state)
{
    (void)state;
    rng_state = 0x2345678u;
    static const int cases[][3] = { { 16, 5, 13 }, { 8, 3, 8 }, { 24, 7, 17 } };
    for (size_t t = 0; t < 3; t++) {
        int C = cases[t][0], H = cases[t][1], W = cases[t][2];
        size_t n = (size_t)C * H * W;
        uint16_t *src = malloc(n * sizeof(uint16_t));
        int32_t *a = malloc(n * sizeof(int32_t));
        int32_t *b = malloc(n * sizeof(int32_t));
        assert_non_null(src);
        for (size_t i = 0; i < n; i++) src[i] = rand_f16_finite();
        ref_nc1hwc2_to_nchw(src, a, C, H, W);
        mlvc_px_nc1hwc2_to_nchw(src, b, C, H, W);
        assert_memory_equal(a, b, n * sizeof(int32_t));
        free(src); free(a); free(b);
    }
}

static void test_nchw_to_nc1hwc2(void **state)
{
    (void)state;
    rng_state = 0x3456789u;
    static const int cases[][3] = { { 16, 4, 11 }, { 8, 2, 8 }, { 24, 6, 9 } };
    for (size_t t = 0; t < 3; t++) {
        int C = cases[t][0], H = cases[t][1], W = cases[t][2];
        size_t n = (size_t)C * H * W;
        int32_t *src = malloc(n * sizeof(int32_t));
        uint16_t *a = malloc(n * sizeof(uint16_t));
        uint16_t *b = malloc(n * sizeof(uint16_t));
        assert_non_null(src);
        for (size_t i = 0; i < n; i++)
            src[i] = (int32_t)(rng() % 60001) - 30000;
        ref_nchw_to_nc1hwc2_fp16(src, a, C, H, W);
        mlvc_px_nchw_to_nc1hwc2_fp16(src, b, C, H, W);
        assert_memory_equal(a, b, n * sizeof(uint16_t));
        free(src); free(a); free(b);
    }
}

/* 融合内核 vs 优化前两段式（NC1HWC2→NCHW + DCR shuffle） */
static void test_d2s_fused(void **state)
{
    (void)state;
    rng_state = 0x456789Au;
    struct { int C1, H, W, C2, bs; } cs[] = {
        { 6, 6, 9, 8, 4 },   /* C=48, oc=3 */
        { 24, 5, 7, 8, 8 },  /* C=192, oc=3（MLVC 640x368 实际形态） */
        { 3, 4, 5, 4, 2 },   /* C=12, oc=3 */
        { 9, 3, 4, 3, 3 },   /* C=27, oc=3 */
    };
    for (size_t t = 0; t < 4; t++) {
        int C1 = cs[t].C1, H = cs[t].H, W = cs[t].W, C2 = cs[t].C2, bs = cs[t].bs;
        int C = C1 * C2;
        size_t n = (size_t)C * H * W;
        size_t on = (size_t)C / (bs * bs) * H * bs * W * bs;
        uint16_t *src = malloc(n * sizeof(uint16_t));
        uint16_t *mid = malloc(n * sizeof(uint16_t));
        uint16_t *a = malloc(on * sizeof(uint16_t));
        uint16_t *b = malloc(on * sizeof(uint16_t));
        assert_non_null(src);
        for (size_t i = 0; i < n; i++) src[i] = (uint16_t)(rng() & 0xffff);
        ref_nc1hwc2_to_nchw_f16(src, mid, C1, H, W, C2);
        ref_depth_to_space_nchw_dcr_f16(mid, a, C, H, W, bs);
        mlvc_px_nc1hwc2_d2s_dcr_f16(src, b, C1, H, W, C2, bs);
        assert_memory_equal(a, b, on * sizeof(uint16_t));
        free(src); free(mid); free(a); free(b);
    }
}

static void test_yuv_to_nhwc(void **state)
{
    (void)state;
    rng_state = 0x56789ABu;
    int W = 9, H = 7, ys = W + 3, uvs = W + 2;
    uint8_t *Y = malloc((size_t)ys * H);
    uint8_t *UV = malloc((size_t)uvs * (H / 2 + 1));
    uint8_t *U = malloc((size_t)uvs * (H / 2 + 1));
    uint8_t *V = malloc((size_t)uvs * (H / 2 + 1));
    assert_non_null(Y);
    for (size_t i = 0; i < (size_t)ys * H; i++) Y[i] = (uint8_t)(rng() & 0xff);
    for (size_t i = 0; i < (size_t)uvs * (H / 2 + 1); i++) {
        UV[i] = (uint8_t)(rng() & 0xff);
        U[i] = (uint8_t)(rng() & 0xff);
        V[i] = (uint8_t)(rng() & 0xff);
    }
    size_t n = (size_t)W * H * 3;
    uint16_t *a = malloc(n * sizeof(uint16_t));
    uint16_t *b = malloc(n * sizeof(uint16_t));
    assert_non_null(a);

    /* NV12：vp = up + 1（与 node_mlvc.c 调用约定一致） */
    ref_yuv_to_nhwc_fp16(Y, ys, UV, UV + 1, uvs, 1, W, H, a);
    mlvc_px_yuv_to_nhwc_fp16(Y, ys, UV, UV + 1, uvs, 1, W, H, b);
    assert_memory_equal(a, b, n * sizeof(uint16_t));

    /* I420：独立平面 */
    ref_yuv_to_nhwc_fp16(Y, ys, U, V, uvs, 0, W, H, a);
    mlvc_px_yuv_to_nhwc_fp16(Y, ys, U, V, uvs, 0, W, H, b);
    assert_memory_equal(a, b, n * sizeof(uint16_t));

    free(Y); free(UV); free(U); free(V); free(a); free(b);
}

/* 饱和转换 vs 优化前 clip(0,1) + ×255 两段语义（含负值/>1 越界输入） */
static void test_nchw_to_nv12(void **state)
{
    (void)state;
    rng_state = 0x6789ABCu;
    int W = 10, H = 6;
    size_t n = 3ull * H * W;
    uint16_t *src = malloc(n * sizeof(uint16_t));
    assert_non_null(src);
    for (size_t i = 0; i < n; i++) {
        uint32_t r = rng() % 100;
        float f;
        if (r < 70) f = (float)(rng() % 256) / 255.0f;       /* [0,1] 常见 */
        else if (r < 85) f = -(float)(rng() % 100) / 50.0f;  /* 负值 */
        else f = 1.0f + (float)(rng() % 100) / 50.0f;        /* >1 */
        src[i] = mlvc_px_f32_to_f16(f);
    }
    int yst = W + 2, uvt = W + 4;
    uint8_t *ya = malloc((size_t)yst * H), *yb = malloc((size_t)yst * H);
    uint8_t *ua = malloc((size_t)uvt * H / 2), *ub = malloc((size_t)uvt * H / 2);
    assert_non_null(ya);
    memset(ya, 0xAA, (size_t)yst * H); memset(yb, 0xAA, (size_t)yst * H);
    memset(ua, 0xAA, (size_t)uvt * H / 2); memset(ub, 0xAA, (size_t)uvt * H / 2);

    uint16_t *clipped = malloc(n * sizeof(uint16_t));
    assert_non_null(clipped);
    memcpy(clipped, src, n * sizeof(uint16_t));
    ref_clip01_f16(clipped, n);
    ref_nchw_yuv_to_nv12(clipped, W, H, ya, yst, ua, uvt);
    mlvc_px_nchw_yuv_fp16_to_nv12_planes(src, W, H, yb, yst, ub, uvt);
    assert_memory_equal(ya, yb, (size_t)yst * H);
    assert_memory_equal(ua, ub, (size_t)uvt * H / 2);
    free(clipped); free(src); free(ya); free(yb); free(ua); free(ub);
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_extract_scales),
        cmocka_unit_test(test_nc1hwc2_to_nchw),
        cmocka_unit_test(test_nchw_to_nc1hwc2),
        cmocka_unit_test(test_d2s_fused),
        cmocka_unit_test(test_yuv_to_nhwc),
        cmocka_unit_test(test_nchw_to_nv12),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
