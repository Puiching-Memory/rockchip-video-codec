/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * lib/mlvc_pixel.c — MLVC CPU 像素/张量转换内核。
 *
 * 每帧热路径（NC1HWC2↔NCHW、DepthToSpace、YUV↔fp16、尺度表展开）。
 * AArch64 走显式 NEON 快速路径（8×8 f16 转置 + 原生舍入转换），
 * 其它架构回退标量实现；两条路径输出逐位一致（见 mlvc_pixel.h 约定）。
 */

#include "mlvc_pixel.h"

#include <math.h>
#include <stdlib.h>

/* ════════════════════════════════════════════════════════════════════ */
/*  extract_scales：z → s0/s1（按 4×4 块展开）                          */
/* ════════════════════════════════════════════════════════════════════ */

#define MLVC_PX_SCALE_MAX_IDX     127
#define MLVC_PX_CHANNEL_REPEAT    4
#define MLVC_PX_SPATIAL_REPEAT    4

void mlvc_px_extract_scales(const int32_t *z_r, int32_t *s0, int32_t *s1,
                            int YC, int YH, int YW, int ZH, int ZW)
{
    for (int c = 0; c < YC; c++) {
        const int32_t *z0 = z_r +
            (size_t)(c / MLVC_PX_CHANNEL_REPEAT) * ZH * ZW;
        const int32_t *z1 = z_r +
            (size_t)((c + YC) / MLVC_PX_CHANNEL_REPEAT) * ZH * ZW;
        int32_t *p0 = s0 + (size_t)c * YH * YW;
        int32_t *p1 = s1 + (size_t)c * YH * YW;

        for (int zy = 0; zy * MLVC_PX_SPATIAL_REPEAT < YH; zy++) {
            int y0 = zy * MLVC_PX_SPATIAL_REPEAT;
            int ye = y0 + MLVC_PX_SPATIAL_REPEAT;
            if (ye > YH) ye = YH;
            const int32_t *z0r = z0 + (size_t)zy * ZW;
            const int32_t *z1r = z1 + (size_t)zy * ZW;

            for (int zx = 0; zx * MLVC_PX_SPATIAL_REPEAT < YW; zx++) {
                int x0 = zx * MLVC_PX_SPATIAL_REPEAT;
                int xe = x0 + MLVC_PX_SPATIAL_REPEAT;
                if (xe > YW) xe = YW;
                /* v0/v1 在 4×4 块内不变：每块算一次（原实现逐元素除法）*/
                int v0 = abs(z0r[zx]);
                int v1 = abs(z1r[zx]);
                if (v0 > MLVC_PX_SCALE_MAX_IDX) v0 = MLVC_PX_SCALE_MAX_IDX;
                if (v1 > MLVC_PX_SCALE_MAX_IDX) v1 = MLVC_PX_SCALE_MAX_IDX;

                /* 满宽块（4 元素整行）：棋盘模式固定，向量化整行写入 */
#ifdef MLVC_PX_NEON
                if (xe - x0 == MLVC_PX_SPATIAL_REPEAT) {
                    int32x2_t h01 = vset_lane_s32(v1, vdup_n_s32(v0), 1);
                    int32x2_t h10 = vset_lane_s32(v0, vdup_n_s32(v1), 1);
                    int32x4_t pat01 = vcombine_s32(h01, h01);
                    int32x4_t pat10 = vcombine_s32(h10, h10);
                    for (int y = y0; y < ye; y++) {
                        int32_t *r0 = p0 + (size_t)y * YW + x0;
                        int32_t *r1 = p1 + (size_t)y * YW + x0;
                        if ((y & 1) == 0) {
                            vst1q_s32(r0, pat01);
                            vst1q_s32(r1, pat10);
                        } else {
                            vst1q_s32(r0, pat10);
                            vst1q_s32(r1, pat01);
                        }
                    }
                    continue;
                }
#endif
                for (int y = y0; y < ye; y++) {
                    int32_t *r0 = p0 + (size_t)y * YW;
                    int32_t *r1 = p1 + (size_t)y * YW;
                    int par = y & 1;
                    for (int x = x0; x < xe; x++) {
                        int chk = (par == (x & 1));
                        r0[x] = chk ? v0 : v1;
                        r1[x] = chk ? v1 : v0;
                    }
                }
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════════ */
/*  NC1HWC2(C2=8) ↔ NCHW                                                */
/* ════════════════════════════════════════════════════════════════════ */

#ifdef MLVC_PX_NEON

/* 8×8 uint16 矩阵转置（8 个像素 × 8 通道 ↔ 8 个通道 × 8 像素）。
 * 三级 trn：16-bit → 32-bit → 64-bit。输入 r[i] = 像素 i 的 8 个通道，
 * 输出 r[k] = 通道 k 的 8 个像素。 */
static inline void transpose_8x8_u16(uint16x8_t r[8])
{
    uint16x8_t t0 = vtrn1q_u16(r[0], r[1]), t1 = vtrn2q_u16(r[0], r[1]);
    uint16x8_t t2 = vtrn1q_u16(r[2], r[3]), t3 = vtrn2q_u16(r[2], r[3]);
    uint16x8_t t4 = vtrn1q_u16(r[4], r[5]), t5 = vtrn2q_u16(r[4], r[5]);
    uint16x8_t t6 = vtrn1q_u16(r[6], r[7]), t7 = vtrn2q_u16(r[6], r[7]);

    uint32x4_t v0 = vtrn1q_u32(vreinterpretq_u32_u16(t0), vreinterpretq_u32_u16(t2));
    uint32x4_t v1 = vtrn2q_u32(vreinterpretq_u32_u16(t0), vreinterpretq_u32_u16(t2));
    uint32x4_t v2 = vtrn1q_u32(vreinterpretq_u32_u16(t1), vreinterpretq_u32_u16(t3));
    uint32x4_t v3 = vtrn2q_u32(vreinterpretq_u32_u16(t1), vreinterpretq_u32_u16(t3));
    uint32x4_t v4 = vtrn1q_u32(vreinterpretq_u32_u16(t4), vreinterpretq_u32_u16(t6));
    uint32x4_t v5 = vtrn2q_u32(vreinterpretq_u32_u16(t4), vreinterpretq_u32_u16(t6));
    uint32x4_t v6 = vtrn1q_u32(vreinterpretq_u32_u16(t5), vreinterpretq_u32_u16(t7));
    uint32x4_t v7 = vtrn2q_u32(vreinterpretq_u32_u16(t5), vreinterpretq_u32_u16(t7));

    /* 实测映射：trn1/2(v0,v4)=ch0/4，trn1/2(v1,v5)=ch2/6，
     * trn1/2(v2,v6)=ch1/5，trn1/2(v3,v7)=ch3/7 */
    r[0] = vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u32(v0), vreinterpretq_u64_u32(v4)));
    r[1] = vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u32(v2), vreinterpretq_u64_u32(v6)));
    r[2] = vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u32(v1), vreinterpretq_u64_u32(v5)));
    r[3] = vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u32(v3), vreinterpretq_u64_u32(v7)));
    r[4] = vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u32(v0), vreinterpretq_u64_u32(v4)));
    r[5] = vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u32(v2), vreinterpretq_u64_u32(v6)));
    r[6] = vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u32(v1), vreinterpretq_u64_u32(v5)));
    r[7] = vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u32(v3), vreinterpretq_u64_u32(v7)));
}

void mlvc_px_nc1hwc2_to_nchw(const uint16_t *src, int32_t *dst,
                             int C, int H, int W)
{
    int C1 = C >> 3;
    for (int c1 = 0; c1 < C1; c1++) {
        for (int y = 0; y < H; y++) {
            const uint16_t *sp = src + (((size_t)c1 * H + y) * W) * 8;
            int32_t *dp[8];
            for (int k = 0; k < 8; k++)
                dp[k] = dst + (((size_t)(c1 * 8 + k) * H + y) * W);

            int x = 0;
            for (; x + 8 <= W; x += 8) {
                uint16x8_t r[8];
                for (int k = 0; k < 8; k++)
                    r[k] = vld1q_u16(sp + k * 8);
                transpose_8x8_u16(r);
                for (int k = 0; k < 8; k++) {
                    float16x8_t h = vreinterpretq_f16_u16(r[k]);
                    /* vcvtnq：round-to-nearest-even，等价 lrintf 默认舍入 */
                    vst1q_s32(dp[k] + x,
                              vcvtnq_s32_f32(vcvt_f32_f16(vget_low_f16(h))));
                    vst1q_s32(dp[k] + x + 4,
                              vcvtnq_s32_f32(vcvt_high_f32_f16(h)));
                }
                sp += 64;
            }
            int xt = x;  /* 尾部像素相对 sp 的偏移 */
            for (; x < W; x++)
                for (int k = 0; k < 8; k++)
                    dp[k][x] = lrintf(mlvc_px_f16_to_f32(sp[(size_t)(x - xt) * 8 + k]));
        }
    }
}

#else  /* 标量回退（与优化前实现一致） */

void mlvc_px_nc1hwc2_to_nchw(const uint16_t *src, int32_t *dst,
                             int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] =
                    lrintf(mlvc_px_f16_to_f32(src[(((size_t)c1 * H + y) * W + x) * 8 + c2]));
    }
}

#endif /* MLVC_PX_NEON */

/* NCHW int32 → NC1HWC2 fp16：两条路径共用标量实现。NEON 8×8 int32 转置版
 * 实测（RK3576，192×92×160）比标量慢约 19%（shuffle 压力），故不采用。 */
void mlvc_px_nchw_to_nc1hwc2_fp16(const int32_t *src, uint16_t *dst,
                                  int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(((size_t)c1 * H + y) * W + x) * 8 + c2] =
                    mlvc_px_f32_to_f16((float)src[((size_t)c * H + y) * W + x]);
    }
}

void mlvc_px_nchw_f16_to_nc1hwc2(const uint16_t *src, uint16_t *dst,
                                 int C, int H, int W, int C2, int w_stride)
{
    if (!src || !dst || C <= 0 || H <= 0 || W <= 0 || C2 <= 0)
        return;
    if (w_stride <= 0)
        w_stride = W;
    if (w_stride < W)
        return;

    int C1 = (C + C2 - 1) / C2;
    memset(dst, 0, (size_t)C1 * H * w_stride * C2 * sizeof(*dst));

#ifdef MLVC_PX_NEON
    if (C2 == 8 && (C & 7) == 0) {
        for (int c1 = 0; c1 < C1; c1++) {
            for (int y = 0; y < H; y++) {
                uint16_t *dp = dst + ((size_t)c1 * H + y) * w_stride * 8;
                const uint16_t *sp[8];
                for (int k = 0; k < 8; k++)
                    sp[k] = src + ((size_t)(c1 * 8 + k) * H + y) * W;

                int x = 0;
                for (; x + 8 <= W; x += 8) {
                    uint16x8_t r[8];
                    for (int k = 0; k < 8; k++)
                        r[k] = vld1q_u16(sp[k] + x);
                    transpose_8x8_u16(r);
                    for (int k = 0; k < 8; k++)
                        vst1q_u16(dp + (size_t)(x + k) * 8, r[k]);
                }
                for (; x < W; x++)
                    for (int k = 0; k < 8; k++)
                        dp[(size_t)x * 8 + k] = sp[k][x];
            }
        }
        return;
    }
#endif

    for (int c = 0; c < C; c++) {
        int c1 = c / C2, c2 = c % C2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(((size_t)c1 * H + y) * w_stride + x) * C2 + c2] =
                    src[((size_t)c * H + y) * W + x];
    }
}

/* ════════════════════════════════════════════════════════════════════ */
/*  NC1HWC2 → DepthToSpace(DCR) 融合                                    */
/* ════════════════════════════════════════════════════════════════════ */

void mlvc_px_nc1hwc2_d2s_dcr_f16(const uint16_t *src, uint16_t *out,
                                 int C1, int H, int W, int C2, int bs)
{
    int C = C1 * C2;
    int oc = C / (bs * bs);
    int oh = H * bs, ow = W * bs;

    for (int c = 0; c < oc; c++) {
        for (int dy = 0; dy < bs; dy++) {
            /* ic = dy*(bs*oc) + dx*oc + c（ONNX DCR），与 w 无关：
             * 每个 dx 的源通道偏移在 h/w 循环外预计算一次 */
            size_t choff[16];
            for (int dx = 0; dx < bs; dx++) {
                int ic = dy * (bs * oc) + dx * oc + c;
                choff[dx] = (size_t)(ic / C2) * H * W * C2 + (size_t)(ic % C2);
            }
            for (int h = 0; h < H; h++) {
                uint16_t *orow = out + ((size_t)c * oh + h * bs + dy) * ow;
                const uint16_t *srow = src + (size_t)h * W * C2;
                for (int w = 0; w < W; w++) {
                    /* 输出行连续写入（原实现输出步长 bs 的散写）*/
                    for (int dx = 0; dx < bs; dx++)
                        orow[w * bs + dx] = srow[(size_t)w * C2 + choff[dx]];
                }
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════════ */
/*  YUV 平面 → NHWC fp16（LUT）                                         */
/* ════════════════════════════════════════════════════════════════════ */

void mlvc_px_yuv_to_nhwc_fp16(const uint8_t *yp, int y_stride,
                              const uint8_t *up, const uint8_t *vp,
                              int uv_stride, int nv12,
                              int W, int H, uint16_t *nhwc)
{
    /* 256 项 LUT：构建表达式与优化前逐像素 (v * (1/255)) 完全一致 */
    uint16_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = mlvc_px_f32_to_f16((float)i * (1.0f / 255.0f));

    int y = 0;
    /* 两行一组：共享一行 UV，UV 索引计算减半 */
    for (; y + 2 <= H; y += 2) {
        const uint8_t *yr0 = yp + (size_t)y * y_stride;
        const uint8_t *yr1 = yr0 + y_stride;
        const uint8_t *uvr = up + (size_t)(y / 2) * uv_stride;
        const uint8_t *vvr = vp + (size_t)(y / 2) * uv_stride;
        uint16_t *d0 = nhwc + (size_t)y * W * 3;
        uint16_t *d1 = d0 + (size_t)W * 3;
        for (int x = 0; x < W; x++) {
            size_t uo = (size_t)(x >> 1) * (nv12 ? 2 : 1);
            uint16_t u = lut[uvr[uo]];
            uint16_t v = lut[vvr[uo]];
            d0[x * 3 + 0] = lut[yr0[x]];
            d0[x * 3 + 1] = u;
            d0[x * 3 + 2] = v;
            d1[x * 3 + 0] = lut[yr1[x]];
            d1[x * 3 + 1] = u;
            d1[x * 3 + 2] = v;
        }
    }
    if (y < H) {  /* 奇数高度尾行 */
        const uint8_t *yr0 = yp + (size_t)y * y_stride;
        const uint8_t *uvr = up + (size_t)(y / 2) * uv_stride;
        const uint8_t *vvr = vp + (size_t)(y / 2) * uv_stride;
        uint16_t *d0 = nhwc + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            size_t uo = (size_t)(x >> 1) * (nv12 ? 2 : 1);
            d0[x * 3 + 0] = lut[yr0[x]];
            d0[x * 3 + 1] = lut[uvr[uo]];
            d0[x * 3 + 2] = lut[vvr[uo]];
        }
    }
}

/* ════════════════════════════════════════════════════════════════════ */
/*  fp16 YUV → NV12 平面（饱和 0..255）                                 */
/* ════════════════════════════════════════════════════════════════════ */

#ifdef MLVC_PX_NEON

/* f16x8 → u8x8：×255 + round-to-nearest-even + 饱和到 0..255。
 * 语义等价 clip(0,1) 后 lrintf(f*255) 再截断饱和。 */
static inline uint8x8_t f16x8_to_u8x8_sat255(uint16x8_t h)
{
    float16x8_t f = vreinterpretq_f16_u16(h);
    float32x4_t lo = vmulq_n_f32(vcvt_f32_f16(vget_low_f16(f)), 255.0f);
    float32x4_t hi = vmulq_n_f32(vcvt_high_f32_f16(f), 255.0f);
    uint16x4_t nlo = vqmovun_s32(vcvtnq_s32_f32(lo));
    uint16x4_t nhi = vqmovun_s32(vcvtnq_s32_f32(hi));
    return vqmovn_u16(vcombine_u16(nlo, nhi));
}

void mlvc_px_nchw_yuv_fp16_to_nv12_planes(const uint16_t *src, int W, int H,
                                          uint8_t *yp, int y_stride,
                                          uint8_t *uv, int uv_stride)
{
    size_t plane = (size_t)H * W;

    for (int y = 0; y < H; y++) {
        const uint16_t *sp = src + (size_t)y * W;
        uint8_t *dp = yp + (size_t)y * y_stride;
        int x = 0;
        for (; x + 8 <= W; x += 8)
            vst1_u8(dp + x, f16x8_to_u8x8_sat255(vld1q_u16(sp + x)));
        for (; x < W; x++) {
            int v = (int)lrintf(mlvc_px_f16_to_f32(sp[x]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t *ur = src + plane + (size_t)(y * 2) * W;
        const uint16_t *vr = src + 2 * plane + (size_t)(y * 2) * W;
        uint8_t *dp = uv + (size_t)y * uv_stride;
        int x = 0;
        for (; x + 8 <= W / 2; x += 8) {
            /* vld2 解交织：偶数列即 2x 采样点 */
            uint16x8x2_t uu = vld2q_u16(ur + x * 2);
            uint16x8x2_t vv = vld2q_u16(vr + x * 2);
            uint8x8x2_t o = { { f16x8_to_u8x8_sat255(uu.val[0]),
                                f16x8_to_u8x8_sat255(vv.val[0]) } };
            vst2_u8(dp + x * 2, o);
        }
        for (; x < W / 2; x++) {
            int u = (int)lrintf(mlvc_px_f16_to_f32(ur[x * 2]) * 255.0f);
            int v = (int)lrintf(mlvc_px_f16_to_f32(vr[x * 2]) * 255.0f);
            dp[x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

#else  /* 标量回退 */

void mlvc_px_nchw_yuv_fp16_to_nv12_planes(const uint16_t *src, int W, int H,
                                          uint8_t *yp, int y_stride,
                                          uint8_t *uv, int uv_stride)
{
    size_t plane = (size_t)H * W;

    for (int y = 0; y < H; y++) {
        const uint16_t *sp = src + (size_t)y * W;
        uint8_t *dp = yp + (size_t)y * y_stride;
        for (int x = 0; x < W; x++) {
            int v = (int)lrintf(mlvc_px_f16_to_f32(sp[x]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t *ur = src + plane + (size_t)(y * 2) * W;
        const uint16_t *vr = src + 2 * plane + (size_t)(y * 2) * W;
        uint8_t *dp = uv + (size_t)y * uv_stride;
        for (int x = 0; x < W / 2; x++) {
            int u = (int)lrintf(mlvc_px_f16_to_f32(ur[x * 2]) * 255.0f);
            int v = (int)lrintf(mlvc_px_f16_to_f32(vr[x * 2]) * 255.0f);
            dp[x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

#endif /* MLVC_PX_NEON */

void mlvc_px_nc1hwc2_fp16_to_nv12_planes(const uint16_t *src, int W, int H,
                                         int c2,
                                         uint8_t *yp, int y_stride,
                                         uint8_t *uv, int uv_stride)
{
    /* 遗留整图 x_hat 模型路径：Y/U/V 以步长 c2 交织，无廉价 gather，
     * 保持标量但按行推进指针（避免逐元素重算乘法）*/
    for (int y = 0; y < H; y++) {
        const uint16_t *sp = src + (size_t)y * W * c2;
        uint8_t *dp = yp + (size_t)y * y_stride;
        for (int x = 0; x < W; x++) {
            int v = (int)lrintf(mlvc_px_f16_to_f32(sp[(size_t)x * c2]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t *sp = src + (size_t)(y * 2) * W * c2;
        uint8_t *dp = uv + (size_t)y * uv_stride;
        for (int x = 0; x < W / 2; x++) {
            const uint16_t *px = sp + (size_t)(x * 2) * c2;
            int u = (int)lrintf(mlvc_px_f16_to_f32(px[1]) * 255.0f);
            int v = (int)lrintf(mlvc_px_f16_to_f32(px[2]) * 255.0f);
            dp[x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}
