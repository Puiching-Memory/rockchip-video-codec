// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sr/post.hpp"

#include <cmath>
#include <cstdint>

namespace sr {
namespace post {

namespace {

uint8_t clip_u8(float v) noexcept {
    if (!(v > 0.0f))
        return 0;
    if (v >= 255.0f)
        return 255;
    return (uint8_t)lrintf(v);
}

uint8_t channel_at(const uint8_t* y_plane, uint32_t y_stride,
                   const uint8_t* uv_plane, uint32_t uv_stride, uint32_t w,
                   uint32_t h, uint32_t ch, uint32_t y, uint32_t x) noexcept {
    if (ch == 0)
        return y_plane[(size_t)y * y_stride + x];
    uint32_t cw = w / 2;
    uint32_t chh = h / 2;
    uint32_t x0 = x / 2;
    uint32_t y0 = y / 2;
    uint32_t x1 = x0 + 1 < cw ? x0 + 1 : x0;
    uint32_t y1 = y0 + 1 < chh ? y0 + 1 : y0;
    uint32_t off = ch - 1;
    int a = uv_plane[(size_t)y0 * uv_stride + x0 * 2 + off];
    if (!(x & 1u) && !(y & 1u))
        return (uint8_t)a;
    int b = uv_plane[(size_t)y0 * uv_stride + x1 * 2 + off];
    int c = uv_plane[(size_t)y1 * uv_stride + x0 * 2 + off];
    if (!(y & 1u))
        return (uint8_t)((a + b + 1) / 2);
    if (!(x & 1u))
        return (uint8_t)((a + c + 1) / 2);
    int d = uv_plane[(size_t)y1 * uv_stride + x1 * 2 + off];
    return (uint8_t)((a + b + c + d + 2) / 4);
}

float cubic_weight(float x) noexcept {
    const float a = -0.5f;
    x = fabsf(x);
    if (x < 1.0f)
        return (a + 2.0f) * x * x * x - (a + 3.0f) * x * x + 1.0f;
    if (x < 2.0f)
        return a * x * x * x - 5.0f * a * x * x + 8.0f * a * x - 4.0f * a;
    return 0.0f;
}

uint32_t clamp_index(int v, uint32_t lim) noexcept {
    if (v < 0)
        return 0;
    if ((uint32_t)v >= lim)
        return lim - 1;
    return (uint32_t)v;
}

void bicubic_channel(const uint8_t* src, uint32_t sw, uint32_t sh,
                     uint32_t s_stride, uint32_t s_px, uint8_t* dst,
                     uint32_t dw, uint32_t dh, uint32_t d_stride,
                     uint32_t d_px) noexcept {
    float sx_scale = (float)sw / (float)dw;
    float sy_scale = (float)sh / (float)dh;
    for (uint32_t y = 0; y < dh; ++y) {
        float sy = ((float)y + 0.5f) * sy_scale - 0.5f;
        int iy = (int)floorf(sy);
        for (uint32_t x = 0; x < dw; ++x) {
            float sx = ((float)x + 0.5f) * sx_scale - 0.5f;
            int ix = (int)floorf(sx);
            float total = 0;
            float weights = 0;
            for (int ky = -1; ky <= 2; ++ky) {
                float wy = cubic_weight(sy - (float)(iy + ky));
                uint32_t py = clamp_index(iy + ky, sh);
                for (int kx = -1; kx <= 2; ++kx) {
                    float wx = cubic_weight(sx - (float)(ix + kx));
                    uint32_t px = clamp_index(ix + kx, sw);
                    float w = wx * wy;
                    total += src[(size_t)py * s_stride +
                                 (size_t)px * s_px] *
                             w;
                    weights += w;
                }
            }
            if (fabsf(weights) > 1e-8f)
                total /= weights;
            dst[(size_t)y * d_stride + (size_t)x * d_px] = clip_u8(total);
        }
    }
}

size_t residual_offset(size_t plane, uint32_t rw, uint32_t ch, uint32_t dy,
                       uint32_t dx, uint32_t cy, uint32_t cx) noexcept {
    uint32_t packed_c = ch * 36 + dy * 6 + dx;
    return (size_t)packed_c * plane + (size_t)cy * rw + cx;
}

}  // namespace

rkvc::Status phase_pack_nv12(const uint8_t* y, uint32_t y_stride,
                              const uint8_t* uv, uint32_t uv_stride,
                              uint32_t w, uint32_t h, uint8_t* phases,
                              size_t phases_size) noexcept {
    if (!y || !uv || !phases || !w || !h || y_stride < w || uv_stride < w ||
        (w % kPhaseInFactor) || (h % kPhaseInFactor))
        return rkvc::Status::Invalid;
    uint32_t cw = w / kPhaseInFactor;
    uint32_t ch = h / kPhaseInFactor;
    if (phases_size < (size_t)kPhaseInCh * cw * ch)
        return rkvc::Status::Invalid;
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t dy = 0; dy < kPhaseInFactor; ++dy)
            for (uint32_t dx = 0; dx < kPhaseInFactor; ++dx) {
                uint32_t pc = c * 4 + dy * 2 + dx;
                for (uint32_t cy = 0; cy < ch; ++cy) {
                    uint32_t sy = cy * 2 + dy;
                    for (uint32_t cx = 0; cx < cw; ++cx) {
                        uint32_t sx = cx * 2 + dx;
                        phases[((size_t)cy * cw + cx) * kPhaseInCh + pc] =
                            channel_at(y, y_stride, uv, uv_stride, w, h, c,
                                       sy, sx);
                    }
                }
            }
    return rkvc::Status::Ok;
}

rkvc::Status bicubic_nv12(const uint8_t* src, uint32_t sw, uint32_t sh,
                           uint32_t s_stride, uint32_t s_vstride,
                           uint8_t* dst, uint32_t dw, uint32_t dh) noexcept {
    if (!src || !dst || !sw || !sh || !dw || !dh || (sw & 1u) || (sh & 1u) ||
        (dw & 1u) || (dh & 1u) || s_stride < sw || s_vstride < sh)
        return rkvc::Status::Invalid;
    const uint8_t* src_uv = src + (size_t)s_stride * s_vstride;
    uint8_t* dst_uv = dst + (size_t)dw * dh;
    bicubic_channel(src, sw, sh, s_stride, 1, dst, dw, dh, dw, 1);
    bicubic_channel(src_uv, sw / 2, sh / 2, s_stride, 2, dst_uv, dw / 2,
                    dh / 2, dw, 2);
    bicubic_channel(src_uv + 1, sw / 2, sh / 2, s_stride, 2, dst_uv + 1,
                    dw / 2, dh / 2, dw, 2);
    return rkvc::Status::Ok;
}

rkvc::Status add_phase_residual(const float* residual, uint32_t rw,
                                uint32_t rh, uint8_t* dst, uint32_t w,
                                uint32_t h) noexcept {
    if (!residual || !dst || w != rw * 6 || h != rh * 6 || (w & 1u) ||
        (h & 1u))
        return rkvc::Status::Invalid;
    size_t plane = (size_t)rw * rh;
    uint8_t* y_plane = dst;
    uint8_t* uv_plane = dst + (size_t)w * h;
    for (uint32_t cy = 0; cy < rh; ++cy)
        for (uint32_t dy = 0; dy < 6; ++dy) {
            uint8_t* row = y_plane + (size_t)(cy * 6 + dy) * w;
            for (uint32_t dx = 0; dx < 6; ++dx) {
                const float* s =
                    residual + residual_offset(plane, rw, 0, dy, dx, cy, 0);
                for (uint32_t cx = 0; cx < rw; ++cx)
                    row[(size_t)cx * 6 + dx] =
                        clip_u8((float)row[(size_t)cx * 6 + dx] + s[cx]);
            }
        }
    for (uint32_t cy = 0; cy < rh; ++cy)
        for (uint32_t ey = 0; ey < 3; ++ey) {
            uint8_t* row = uv_plane + (size_t)(cy * 3 + ey) * w;
            for (uint32_t ex = 0; ex < 3; ++ex)
                for (uint32_t ch = 1; ch <= 2; ++ch) {
                    const float* s0 =
                        residual +
                        residual_offset(plane, rw, ch, ey * 2, ex * 2, cy, 0);
                    const float* s1 = s0 + plane;
                    const float* s2 = s0 + 6 * plane;
                    const float* s3 = s2 + plane;
                    uint32_t off = ex * 2 + ch - 1;
                    for (uint32_t cx = 0; cx < rw; ++cx) {
                        uint8_t* px = row + (size_t)cx * 6 + off;
                        float v = (s0[cx] + s1[cx] + s2[cx] + s3[cx]) * 0.25f;
                        *px = clip_u8((float)*px + v);
                    }
                }
        }
    return rkvc::Status::Ok;
}

}  // namespace post
}  // namespace sr
