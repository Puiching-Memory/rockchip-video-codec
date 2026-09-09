// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/pixel.hpp"

#include <cmath>
#include <cstring>

namespace mlvc {
namespace pixel {

namespace {

int clamp_abs_scale(int32_t value, int max_index) noexcept {
    int64_t mag = value < 0 ? -(int64_t)value : (int64_t)value;
    return mag > max_index ? max_index : (int)mag;
}

#ifdef MLVC_PX_NEON
inline void transpose_8x8_u16(uint16x8_t r[8]) noexcept {
    uint16x8_t t0 = vtrn1q_u16(r[0], r[1]), t1 = vtrn2q_u16(r[0], r[1]);
    uint16x8_t t2 = vtrn1q_u16(r[2], r[3]), t3 = vtrn2q_u16(r[2], r[3]);
    uint16x8_t t4 = vtrn1q_u16(r[4], r[5]), t5 = vtrn2q_u16(r[4], r[5]);
    uint16x8_t t6 = vtrn1q_u16(r[6], r[7]), t7 = vtrn2q_u16(r[6], r[7]);

    uint32x4_t v0 =
        vtrn1q_u32(vreinterpretq_u32_u16(t0), vreinterpretq_u32_u16(t2));
    uint32x4_t v1 =
        vtrn2q_u32(vreinterpretq_u32_u16(t0), vreinterpretq_u32_u16(t2));
    uint32x4_t v2 =
        vtrn1q_u32(vreinterpretq_u32_u16(t1), vreinterpretq_u32_u16(t3));
    uint32x4_t v3 =
        vtrn2q_u32(vreinterpretq_u32_u16(t1), vreinterpretq_u32_u16(t3));
    uint32x4_t v4 =
        vtrn1q_u32(vreinterpretq_u32_u16(t4), vreinterpretq_u32_u16(t6));
    uint32x4_t v5 =
        vtrn2q_u32(vreinterpretq_u32_u16(t4), vreinterpretq_u32_u16(t6));
    uint32x4_t v6 =
        vtrn1q_u32(vreinterpretq_u32_u16(t5), vreinterpretq_u32_u16(t7));
    uint32x4_t v7 =
        vtrn2q_u32(vreinterpretq_u32_u16(t5), vreinterpretq_u32_u16(t7));

    r[0] = vreinterpretq_u16_u64(
        vtrn1q_u64(vreinterpretq_u64_u32(v0), vreinterpretq_u64_u32(v4)));
    r[1] = vreinterpretq_u16_u64(
        vtrn1q_u64(vreinterpretq_u64_u32(v2), vreinterpretq_u64_u32(v6)));
    r[2] = vreinterpretq_u16_u64(
        vtrn1q_u64(vreinterpretq_u64_u32(v1), vreinterpretq_u64_u32(v5)));
    r[3] = vreinterpretq_u16_u64(
        vtrn1q_u64(vreinterpretq_u64_u32(v3), vreinterpretq_u64_u32(v7)));
    r[4] = vreinterpretq_u16_u64(
        vtrn2q_u64(vreinterpretq_u64_u32(v0), vreinterpretq_u64_u32(v4)));
    r[5] = vreinterpretq_u16_u64(
        vtrn2q_u64(vreinterpretq_u64_u32(v2), vreinterpretq_u64_u32(v6)));
    r[6] = vreinterpretq_u16_u64(
        vtrn2q_u64(vreinterpretq_u64_u32(v1), vreinterpretq_u64_u32(v5)));
    r[7] = vreinterpretq_u16_u64(
        vtrn2q_u64(vreinterpretq_u64_u32(v3), vreinterpretq_u64_u32(v7)));
}

inline uint8x8_t f16x8_to_u8x8_sat255(uint16x8_t h) noexcept {
    float16x8_t f = vreinterpretq_f16_u16(h);
    float32x4_t lo = vmulq_n_f32(vcvt_f32_f16(vget_low_f16(f)), 255.0f);
    float32x4_t hi = vmulq_n_f32(vcvt_high_f32_f16(f), 255.0f);
    uint16x4_t nlo = vqmovun_s32(vcvtnq_s32_f32(lo));
    uint16x4_t nhi = vqmovun_s32(vcvtnq_s32_f32(hi));
    return vqmovn_u16(vcombine_u16(nlo, nhi));
}
#endif

}  // namespace

rkvc::Status extract_scales(const int32_t* z_r, int32_t* s0, int32_t* s1,
                            int YC, int YH, int YW, int ZC, int ZH, int ZW,
                            int channel_repeat, int spatial_repeat,
                            int scale_max_index) noexcept {
    if (!z_r || !s0 || !s1 || YC <= 0 || YH <= 0 || YW <= 0 || ZC <= 0 ||
        ZH <= 0 || ZW <= 0 || channel_repeat <= 0 || spatial_repeat <= 0 ||
        scale_max_index < 0)
        return rkvc::Status::Invalid;
    int64_t need_z = (static_cast<int64_t>(2) * YC + channel_repeat - 1) /
                     channel_repeat;
    if (need_z > ZC)
        return rkvc::Status::Invalid;

    for (int c = 0; c < YC; c++) {
        const int32_t* z0 =
            z_r + static_cast<size_t>(c / channel_repeat) * ZH * ZW;
        const int32_t* z1 =
            z_r + static_cast<size_t>((c + YC) / channel_repeat) * ZH * ZW;
        int32_t* p0 = s0 + static_cast<size_t>(c) * YH * YW;
        int32_t* p1 = s1 + static_cast<size_t>(c) * YH * YW;

        for (int y0 = 0; y0 < YH; y0 += spatial_repeat) {
            int zy = y0 / spatial_repeat;
            int ye = y0 + spatial_repeat;
            if (ye > YH)
                ye = YH;
            if (zy >= ZH)
                zy = ZH - 1;
            const int32_t* z0r = z0 + static_cast<size_t>(zy) * ZW;
            const int32_t* z1r = z1 + static_cast<size_t>(zy) * ZW;

            for (int x0 = 0; x0 < YW; x0 += spatial_repeat) {
                int zx = x0 / spatial_repeat;
                int xe = x0 + spatial_repeat;
                if (xe > YW)
                    xe = YW;
                if (zx >= ZW)
                    zx = ZW - 1;
                int v0 = clamp_abs_scale(z0r[zx], scale_max_index);
                int v1 = clamp_abs_scale(z1r[zx], scale_max_index);

#ifdef MLVC_PX_NEON
                int32x2_t h01 = vset_lane_s32(v1, vdup_n_s32(v0), 1);
                int32x2_t h10 = vset_lane_s32(v0, vdup_n_s32(v1), 1);
                int32x4_t pat01 = vcombine_s32(h01, h01);
                int32x4_t pat10 = vcombine_s32(h10, h10);
                for (int y = y0; y < ye; y++) {
                    int32_t* r0 = p0 + static_cast<size_t>(y) * YW;
                    int32_t* r1 = p1 + static_cast<size_t>(y) * YW;
                    int x = x0;
                    for (; x + 4 <= xe; x += 4) {
                        if (((y ^ x) & 1) == 0) {
                            vst1q_s32(r0 + x, pat01);
                            vst1q_s32(r1 + x, pat10);
                        } else {
                            vst1q_s32(r0 + x, pat10);
                            vst1q_s32(r1 + x, pat01);
                        }
                    }
                    for (; x < xe; x++) {
                        int chk = ((y & 1) == (x & 1));
                        r0[x] = chk ? v0 : v1;
                        r1[x] = chk ? v1 : v0;
                    }
                }
#else
                for (int y = y0; y < ye; y++) {
                    int32_t* r0 = p0 + static_cast<size_t>(y) * YW;
                    int32_t* r1 = p1 + static_cast<size_t>(y) * YW;
                    for (int x = x0; x < xe; x++) {
                        int chk = ((y & 1) == (x & 1));
                        r0[x] = chk ? v0 : v1;
                        r1[x] = chk ? v1 : v0;
                    }
                }
#endif
            }
        }
    }
    return rkvc::Status::Ok;
}

void detail::nc1hwc2_to_nchw_scalar(const uint16_t* src, int32_t* dst, int C,
                                    int H, int W) noexcept {
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] = lrintf(f16_to_f32(
                    src[(((size_t)c1 * H + y) * W + x) * 8 + c2]));
    }
}

void nc1hwc2_to_nchw(const uint16_t* src, int32_t* dst, int C, int H,
                     int W) noexcept {
#ifdef MLVC_PX_NEON
    int C1 = C >> 3;
    for (int c1 = 0; c1 < C1; c1++) {
        for (int y = 0; y < H; y++) {
            const uint16_t* sp = src + (((size_t)c1 * H + y) * W) * 8;
            int32_t* dp[8];
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
                    vst1q_s32(dp[k] + x,
                              vcvtnq_s32_f32(vcvt_f32_f16(vget_low_f16(h))));
                    vst1q_s32(dp[k] + x + 4,
                              vcvtnq_s32_f32(vcvt_high_f32_f16(h)));
                }
                sp += 64;
            }
            int xt = x;
            for (; x < W; x++)
                for (int k = 0; k < 8; k++)
                    dp[k][x] =
                        lrintf(f16_to_f32(sp[(size_t)(x - xt) * 8 + k]));
        }
    }
#else
    detail::nc1hwc2_to_nchw_scalar(src, dst, C, H, W);
#endif
}

void nchw_to_nc1hwc2_fp16(const int32_t* src, uint16_t* dst, int C, int H,
                           int W) noexcept {
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(((size_t)c1 * H + y) * W + x) * 8 + c2] =
                    f32_to_f16((float)src[((size_t)c * H + y) * W + x]);
    }
}

void nchw_i32_to_nhwc_f16(const int32_t* src, uint16_t* dst, int C, int H,
                           int W) noexcept {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < C; c++)
                dst[((size_t)y * W + x) * C + c] =
                    f32_to_f16((float)src[((size_t)c * H + y) * W + x]);
}

void nchw_f16_to_nhwc(const uint16_t* src, uint16_t* dst, int C, int H,
                       int W) noexcept {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            for (int c = 0; c < C; c++)
                dst[((size_t)y * W + x) * C + c] =
                    src[((size_t)c * H + y) * W + x];
}

void nchw_d2s_dcr_f16(const uint16_t* src, uint16_t* dst, int OC, int H,
                       int W, int bs) noexcept {
    int OH = H * bs, OW = W * bs;
    for (int c = 0; c < OC; ++c)
        for (int y = 0; y < OH; ++y)
            for (int x = 0; x < OW; ++x) {
                int dy = y % bs, dx = x % bs;
                dst[((size_t)c * OH + y) * OW + x] =
                    src[(((size_t)(dy * bs + dx) * OC + c) * H + y / bs) *
                            W +
                        x / bs];
            }
}

void nchw_f16_to_nc1hwc2(const uint16_t* src, uint16_t* dst, int C, int H,
                          int W, int C2, int w_stride) noexcept {
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
                uint16_t* dp = dst + ((size_t)c1 * H + y) * w_stride * 8;
                const uint16_t* sp[8];
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

void nc1hwc2_d2s_dcr_f16(const uint16_t* src, uint16_t* out, int C1, int H,
                         int W, int C2, int bs) noexcept {
    int C = C1 * C2;
    int oc = C / (bs * bs);
    int oh = H * bs, ow = W * bs;

    for (int c = 0; c < oc; c++) {
        for (int dy = 0; dy < bs; dy++) {
            size_t choff[16];
            for (int dx = 0; dx < bs; dx++) {
                int ic = dy * (bs * oc) + dx * oc + c;
                choff[dx] =
                    (size_t)(ic / C2) * H * W * C2 + (size_t)(ic % C2);
            }
            for (int h = 0; h < H; h++) {
                uint16_t* orow = out + ((size_t)c * oh + h * bs + dy) * ow;
                const uint16_t* srow = src + (size_t)h * W * C2;
                for (int w = 0; w < W; w++)
                    for (int dx = 0; dx < bs; dx++)
                        orow[w * bs + dx] = srow[(size_t)w * C2 + choff[dx]];
            }
        }
    }
}

void yuv_to_nhwc_fp16(const uint8_t* yp, int y_stride, const uint8_t* up,
                       const uint8_t* vp, int uv_stride, int nv12, int W,
                       int H, uint16_t* nhwc) noexcept {
    uint16_t lut[256];
    for (int i = 0; i < 256; i++)
        lut[i] = f32_to_f16((float)i * (1.0f / 255.0f));

    int y = 0;
    for (; y + 2 <= H; y += 2) {
        const uint8_t* yr0 = yp + (size_t)y * y_stride;
        const uint8_t* yr1 = yr0 + y_stride;
        const uint8_t* uvr = up + (size_t)(y / 2) * uv_stride;
        const uint8_t* vvr = vp + (size_t)(y / 2) * uv_stride;
        uint16_t* d0 = nhwc + (size_t)y * W * 3;
        uint16_t* d1 = d0 + (size_t)W * 3;
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
    if (y < H) {
        const uint8_t* yr0 = yp + (size_t)y * y_stride;
        const uint8_t* uvr = up + (size_t)(y / 2) * uv_stride;
        const uint8_t* vvr = vp + (size_t)(y / 2) * uv_stride;
        uint16_t* d0 = nhwc + (size_t)y * W * 3;
        for (int x = 0; x < W; x++) {
            size_t uo = (size_t)(x >> 1) * (nv12 ? 2 : 1);
            d0[x * 3 + 0] = lut[yr0[x]];
            d0[x * 3 + 1] = lut[uvr[uo]];
            d0[x * 3 + 2] = lut[vvr[uo]];
        }
    }
}

void detail::nchw_yuv_fp16_to_nv12_planes_scalar(const uint16_t* src, int W,
                                                 int H, uint8_t* yp,
                                                 int y_stride, uint8_t* uv,
                                                 int uv_stride) noexcept {
    size_t plane = (size_t)H * W;
    for (int y = 0; y < H; y++) {
        const uint16_t* sp = src + (size_t)y * W;
        uint8_t* dp = yp + (size_t)y * y_stride;
        for (int x = 0; x < W; x++) {
            int v = (int)lrintf(f16_to_f32(sp[x]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t* ur = src + plane + (size_t)(y * 2) * W;
        const uint16_t* vr = src + 2 * plane + (size_t)(y * 2) * W;
        uint8_t* dp = uv + (size_t)y * uv_stride;
        for (int x = 0; x < W / 2; x++) {
            int u = (int)lrintf(f16_to_f32(ur[x * 2]) * 255.0f);
            int v = (int)lrintf(f16_to_f32(vr[x * 2]) * 255.0f);
            dp[x * 2] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

void nchw_yuv_fp16_to_nv12_planes(const uint16_t* src, int W, int H,
                                   uint8_t* yp, int y_stride, uint8_t* uv,
                                   int uv_stride) noexcept {
#ifdef MLVC_PX_NEON
    size_t plane = (size_t)H * W;
    for (int y = 0; y < H; y++) {
        const uint16_t* sp = src + (size_t)y * W;
        uint8_t* dp = yp + (size_t)y * y_stride;
        int x = 0;
        for (; x + 8 <= W; x += 8)
            vst1_u8(dp + x, f16x8_to_u8x8_sat255(vld1q_u16(sp + x)));
        for (; x < W; x++) {
            int v = (int)lrintf(f16_to_f32(sp[x]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t* ur = src + plane + (size_t)(y * 2) * W;
        const uint16_t* vr = src + 2 * plane + (size_t)(y * 2) * W;
        uint8_t* dp = uv + (size_t)y * uv_stride;
        int x = 0;
        for (; x + 8 <= W / 2; x += 8) {
            uint16x8x2_t uu = vld2q_u16(ur + x * 2);
            uint16x8x2_t vv = vld2q_u16(vr + x * 2);
            uint8x8x2_t o = {{f16x8_to_u8x8_sat255(uu.val[0]),
                              f16x8_to_u8x8_sat255(vv.val[0])}};
            vst2_u8(dp + x * 2, o);
        }
        for (; x < W / 2; x++) {
            int u = (int)lrintf(f16_to_f32(ur[x * 2]) * 255.0f);
            int v = (int)lrintf(f16_to_f32(vr[x * 2]) * 255.0f);
            dp[x * 2] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
#else
    detail::nchw_yuv_fp16_to_nv12_planes_scalar(src, W, H, yp, y_stride, uv,
                                                uv_stride);
#endif
}

void nc1hwc2_fp16_to_nv12_planes(const uint16_t* src, int W, int H, int c2,
                                  uint8_t* yp, int y_stride, uint8_t* uv,
                                  int uv_stride) noexcept {
    for (int y = 0; y < H; y++) {
        const uint16_t* sp = src + (size_t)y * W * c2;
        uint8_t* dp = yp + (size_t)y * y_stride;
        for (int x = 0; x < W; x++) {
            int v =
                (int)lrintf(f16_to_f32(sp[(size_t)x * c2]) * 255.0f);
            dp[x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        const uint16_t* sp = src + (size_t)(y * 2) * W * c2;
        uint8_t* dp = uv + (size_t)y * uv_stride;
        for (int x = 0; x < W / 2; x++) {
            const uint16_t* px = sp + (size_t)(x * 2) * c2;
            int u = (int)lrintf(f16_to_f32(px[1]) * 255.0f);
            int v = (int)lrintf(f16_to_f32(px[2]) * 255.0f);
            dp[x * 2] = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            dp[x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

}  // namespace pixel
}  // namespace mlvc
