// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "rkvc/status.hpp"

#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(__aarch64__)
#include <arm_neon.h>
#define MLVC_PX_NEON 1
#endif

namespace mlvc {
namespace pixel {

#ifdef MLVC_PX_NEON
inline uint16_t f32_to_f16(float f) noexcept {
    __fp16 h = static_cast<__fp16>(f);
    uint16_t r = 0;
    memcpy(&r, &h, 2);
    return r;
}
inline float f16_to_f32(uint16_t h) noexcept {
    __fp16 p = 0;
    memcpy(&p, &h, 2);
    return static_cast<float>(p);
}
#else
inline uint16_t f32_to_f16(float f) noexcept {
    uint32_t x = 0;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10)
            return static_cast<uint16_t>(sign);
        mant |= 0x800000;
        uint32_t shift = static_cast<uint32_t>(14 - exp);
        uint32_t half = 1u << (shift - 1);
        uint32_t mant16 = mant >> shift;
        if ((mant & (half * 2 - 1)) > half ||
            ((mant & (half * 2 - 1)) == half && (mant16 & 1)))
            ++mant16;
        return static_cast<uint16_t>(sign | mant16);
    }
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7c00);
    uint32_t mant16 = mant >> 13;
    if ((mant & 0x1fff) > 0x1000 ||
        ((mant & 0x1fff) == 0x1000 && (mant16 & 1)))
        ++mant16;
    if (mant16 == 0x400) {
        mant16 = 0;
        ++exp;
        if (exp >= 31)
            return static_cast<uint16_t>(sign | 0x7c00);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                 mant16);
}
inline float f16_to_f32(uint16_t h) noexcept {
    uint32_t sign = static_cast<uint32_t>(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t out = 0;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                --exp;
            }
            mant &= 0x3ff;
            out = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 0x1f) {
        out = sign | 0x7f800000 | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f = 0;
    memcpy(&f, &out, 4);
    return f;
}
#endif

// z_raw -> checkerboard scale tables s0/s1. Returns Ok or Invalid.
rkvc::Status extract_scales(const int32_t* z_r, int32_t* s0, int32_t* s1,
                            int YC, int YH, int YW, int ZC, int ZH, int ZW,
                            int channel_repeat, int spatial_repeat,
                            int scale_max_index) noexcept;

// NC1HWC2(C2=8) fp16 -> NCHW int32 (lrintf rounding). C: multiple of 8.
void nc1hwc2_to_nchw(const uint16_t* src, int32_t* dst, int C, int H,
                     int W) noexcept;
// NCHW int32 -> NC1HWC2(C2=8) fp16.
void nchw_to_nc1hwc2_fp16(const int32_t* src, uint16_t* dst, int C, int H,
                           int W) noexcept;
// NCHW fp16 -> NC1HWC2 fp16, bit-preserving. w_stride 0 means W.
void nchw_f16_to_nc1hwc2(const uint16_t* src, uint16_t* dst, int C, int H,
                          int W, int C2, int w_stride) noexcept;
// NC1HWC2 -> ONNX DepthToSpace(DCR) fused, NCHW fp16 out.
void nc1hwc2_d2s_dcr_f16(const uint16_t* src, uint16_t* out, int C1, int H,
                         int W, int C2, int bs) noexcept;
// NCHW int32 -> NHWC fp16 (host I/O path: NPU takes NHWC, returns NCHW).
void nchw_i32_to_nhwc_f16(const int32_t* src, uint16_t* dst, int C, int H,
                           int W) noexcept;
// NCHW fp16 -> NHWC fp16, bit-preserving reorder.
void nchw_f16_to_nhwc(const uint16_t* src, uint16_t* dst, int C, int H,
                       int W) noexcept;
// NCHW fp16 DepthToSpace(DCR): [oc*bs*bs,h,w] -> [oc,h*bs,w*bs].
void nchw_d2s_dcr_f16(const uint16_t* src, uint16_t* dst, int OC, int H,
                       int W, int bs) noexcept;
// YUV planes (NV12 or I420) -> NHWC fp16 (x1/255).
void yuv_to_nhwc_fp16(const uint8_t* yp, int y_stride, const uint8_t* up,
                       const uint8_t* vp, int uv_stride, int nv12, int W,
                       int H, uint16_t* nhwc) noexcept;
// NCHW fp16 YUV (3 planes) -> NV12 u8.
void nchw_yuv_fp16_to_nv12_planes(const uint16_t* src, int W, int H,
                                   uint8_t* yp, int y_stride, uint8_t* uv,
                                   int uv_stride) noexcept;
// NC1HWC2 fp16 (Y/U/V first) -> NV12 u8.
void nc1hwc2_fp16_to_nv12_planes(const uint16_t* src, int W, int H, int c2,
                                  uint8_t* yp, int y_stride, uint8_t* uv,
                                  int uv_stride) noexcept;

namespace detail {
// Scalar references, always compiled: the board cross-check runs these
// against the NEON paths for bitwise equality.
void nc1hwc2_to_nchw_scalar(const uint16_t* src, int32_t* dst, int C, int H,
                            int W) noexcept;
void nchw_yuv_fp16_to_nv12_planes_scalar(const uint16_t* src, int W, int H,
                                         uint8_t* yp, int y_stride,
                                         uint8_t* uv,
                                         int uv_stride) noexcept;
}  // namespace detail

}  // namespace pixel
}  // namespace mlvc
