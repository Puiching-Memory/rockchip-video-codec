/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rknn_sr_neon.c
 * @brief NEON 加速的 RKNN SR 量化/反量化。
 */

#include "rkvc_sr_neon.h"

#include <math.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define RKVC_SR_NEON 1
#endif

static int sr_affine_is_byte_shift(int32_t zp, float scale)
{
    return zp == -128 && fabsf(scale - 1.f) < 0.01f;
}

void rkvc_sr_quant_rgb24_nhwc_to_int8(const uint8_t *rgb, int8_t *out,
                                      size_t n, int32_t zp, float scale)
{
    if (!rgb || !out || n == 0)
        return;

    if (sr_affine_is_byte_shift(zp, scale)) {
#ifdef RKVC_SR_NEON
        size_t i = 0;
        const int8x16_t bias = vdupq_n_s8((int8_t)-128);

        for (; i + 16 <= n; i += 16) {
            uint8x16_t v = vld1q_u8(rgb + i);
            int8x16_t q = vreinterpretq_s8_u8(vaddq_u8(v, vreinterpretq_u8_s8(bias)));
            vst1q_s8(out + i, q);
        }
        for (; i < n; i++)
            out[i] = (int8_t)((int)rgb[i] - 128);
#else
        for (size_t i = 0; i < n; i++)
            out[i] = (int8_t)((int)rgb[i] - 128);
#endif
        return;
    }

    const float inv = 1.f / scale;
    for (size_t i = 0; i < n; i++) {
        int q = (int)lrintf((float)rgb[i] * inv) + zp;
        if (q < -128)
            q = -128;
        else if (q > 127)
            q = 127;
        out[i] = (int8_t)q;
    }
}

void rkvc_sr_dequant_nchw_int8_to_rgb24(const int8_t *nchw, int w, int h,
                                        int32_t zp, float scale, uint8_t *rgb,
                                        int rgb_stride)
{
    if (!nchw || !rgb || w <= 0 || h <= 0 || rgb_stride < w * 3)
        return;

    const int hw = w * h;
    const int8_t *r_plane = nchw + 0 * hw;
    const int8_t *g_plane = nchw + 1 * hw;
    const int8_t *b_plane = nchw + 2 * hw;

    if (sr_affine_is_byte_shift(zp, scale)) {
#ifdef RKVC_SR_NEON
        const int8x16_t bias = vdupq_n_s8((int8_t)128);

        for (int y = 0; y < h; y++) {
            uint8_t *row = rgb + y * rgb_stride;
            const int row_base = y * w;
            int x = 0;

            for (; x + 16 <= w; x += 16) {
                const int i = row_base + x;
                int8x16_t rv = vld1q_s8(r_plane + i);
                int8x16_t gv = vld1q_s8(g_plane + i);
                int8x16_t bv = vld1q_s8(b_plane + i);

                uint8x16x3_t pix;
                pix.val[0] = vreinterpretq_u8_s8(vaddq_s8(rv, bias));
                pix.val[1] = vreinterpretq_u8_s8(vaddq_s8(gv, bias));
                pix.val[2] = vreinterpretq_u8_s8(vaddq_s8(bv, bias));
                vst3q_u8(row + x * 3, pix);
            }
            for (; x < w; x++) {
                const int i = row_base + x;
                row[x * 3 + 0] = (uint8_t)((int)r_plane[i] + 128);
                row[x * 3 + 1] = (uint8_t)((int)g_plane[i] + 128);
                row[x * 3 + 2] = (uint8_t)((int)b_plane[i] + 128);
            }
        }
#else
        for (int i = 0; i < hw; i++) {
            rgb[i * 3 + 0] = (uint8_t)((int)r_plane[i] + 128);
            rgb[i * 3 + 1] = (uint8_t)((int)g_plane[i] + 128);
            rgb[i * 3 + 2] = (uint8_t)((int)b_plane[i] + 128);
        }
#endif
        return;
    }

    for (int y = 0; y < h; y++) {
        uint8_t *row = rgb + y * rgb_stride;
        const int row_base = y * w;
        for (int x = 0; x < w; x++) {
            const int i = row_base + x;
            float rv = ((float)r_plane[i] - (float)zp) * scale;
            float gv = ((float)g_plane[i] - (float)zp) * scale;
            float bv = ((float)b_plane[i] - (float)zp) * scale;
            if (rv < 0.f) rv = 0.f; else if (rv > 255.f) rv = 255.f;
            if (gv < 0.f) gv = 0.f; else if (gv > 255.f) gv = 255.f;
            if (bv < 0.f) bv = 0.f; else if (bv > 255.f) bv = 255.f;
            row[x * 3 + 0] = (uint8_t)(rv + 0.5f);
            row[x * 3 + 1] = (uint8_t)(gv + 0.5f);
            row[x * 3 + 2] = (uint8_t)(bv + 0.5f);
        }
    }
}
