/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file mlvc_pixel.h
 * @brief MLVC CPU 像素/张量转换内核（内部）。
 *
 * 每帧热路径纯数组内核，便于 NEON 优化与独立单测。
 * 所有内核只依赖裸指针与尺寸，不依赖 rkvc_frame / RKNN。
 *
 * 逐位等价约定：NEON 快速路径与标量参考实现输出逐位一致——
 *   - f32→f16 用 ARM 原生转换（round-to-nearest-even），与 __fp16 转换一致；
 *   - f16/f32→int 舍入用 vcvtnq（round-to-nearest-even），等价 lrintf 默认舍入。
 * 非 ARM NEON 平台回退标量实现，行为与优化前完全一致。
 */

#ifndef RKVC_BACKEND_MLVC_PIXEL_H
#define RKVC_BACKEND_MLVC_PIXEL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define MLVC_PX_NEON 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MLVC_PX_NEON)

static inline uint16_t mlvc_px_f32_to_f16(float f)
{
    __fp16 h = (__fp16)f;
    uint16_t r;
    memcpy(&r, &h, 2);
    return r;
}

static inline float mlvc_px_f16_to_f32(uint16_t h)
{
    __fp16 p;
    memcpy(&p, &h, 2);
    return (float)p;
}

#else  /* 可移植软件实现 */

static inline uint16_t mlvc_px_f32_to_f16(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        /* 次正规：round-to-nearest-even */
        mant |= 0x800000;
        uint32_t shift = (uint32_t)(14 - exp);
        uint32_t half = 1u << (shift - 1);
        uint32_t mant16 = mant >> shift;
        if ((mant & (half * 2 - 1)) > half ||
            ((mant & (half * 2 - 1)) == half && (mant16 & 1)))
            mant16++;
        return (uint16_t)(sign | mant16);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    uint32_t mant16 = mant >> 13;
    if ((mant & 0x1fff) > 0x1000 || ((mant & 0x1fff) == 0x1000 && (mant16 & 1)))
        mant16++;
    if (mant16 == 0x400) {
        mant16 = 0;
        exp++;
        if (exp >= 31) return (uint16_t)(sign | 0x7c00);
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | mant16);
}

static inline float mlvc_px_f16_to_f32(uint16_t h)
{
    uint32_t sign = (uint32_t)(h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t out;
    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            /* 次正规归一化 */
            exp = 127 - 15 + 1;
            while (!(mant & 0x400)) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3ff;
            out = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 0x1f) {
        out = sign | 0x7f800000 | (mant << 13);
    } else {
        out = sign | ((exp - 15 + 127) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &out, 4);
    return f;
}

#endif /* MLVC_PX_NEON */

/**
 * @brief z_raw → 棋盘格尺度表 s0/s1（MLVC 熵编码的分布索引）。
 *
 * 标准 MLVC 使用 channel_repeat=2 / spatial_repeat=8，MLVC-S 使用
 * 4 / 4。参数必须与导出模型一致；函数会校验 z 通道上界，并把
 * 空间坐标夹到 z 边界，与上游 repeat_interleave 语义一致。
 *
 * @return 0 成功，-1 表示指针、尺寸或尺度配置无效。
 */
int mlvc_px_extract_scales(const int32_t *z_r, int32_t *s0, int32_t *s1,
                           int YC, int YH, int YW, int ZC, int ZH, int ZW,
                           int channel_repeat, int spatial_repeat,
                           int scale_max_index);

/**
 * @brief NC1HWC2(C2=8) fp16 → NCHW int32（lrintf 舍入）。
 * @param C 通道总数，必须为 8 的倍数。
 */
void mlvc_px_nc1hwc2_to_nchw(const uint16_t *src, int32_t *dst,
                             int C, int H, int W);

/**
 * @brief NCHW int32 → NC1HWC2(C2=8) fp16。
 * @param C 通道总数，必须为 8 的倍数。
 */
void mlvc_px_nchw_to_nc1hwc2_fp16(const int32_t *src, uint16_t *dst,
                                  int C, int H, int W);

/**
 * @brief NCHW fp16 → NC1HWC2 fp16，保留 fp16 位模式。
 *
 * 用于把 RKNN 标准输出的逻辑 NCHW feature 直接打包到
 * native reference 输入内存。函数会清零通道和行 stride 填充。
 * @param C 逻辑通道数。
 * @param C2 native 组内通道数（RKNN fp16 通常为 8）。
 * @param w_stride native 宽度 stride，0 表示等于 W。
 */
void mlvc_px_nchw_f16_to_nc1hwc2(const uint16_t *src, uint16_t *dst,
                                 int C, int H, int W, int C2, int w_stride);

/**
 * @brief NC1HWC2 → ONNX DepthToSpace(mode=DCR) 融合内核，直接输出 NCHW fp16。
 *
 * 等价于先 NC1HWC2→NCHW 再 DCR shuffle，但省去中间张量的一读一写。
 * @param C1 NC1HWC2 的块通道数（总通道 = C1*C2）。
 * @param C2 NC1HWC2 的组内通道数（通常为 8）。
 * @param bs DepthToSpace blocksize；须满足 C1*C2 = oc*bs*bs（oc 为输出通道数）。
 */
void mlvc_px_nc1hwc2_d2s_dcr_f16(const uint16_t *src, uint16_t *out,
                                 int C1, int H, int W, int C2, int bs);

/**
 * @brief YUV 三平面（NV12 或 I420）→ NHWC fp16（值域 ×1/255）。
 *
 * LUT 化的整数→fp16 转换与原浮点表达式 (v * (1/255)) 逐位一致；
 * 相邻两行共享一行 UV，UV 索引开销减半。
 * @param nv12 1 时 UV 平面交织（up/vp 指向同一缓冲的 U/V 字节）；
 *             0 时 up/vp 为独立平面。
 */
void mlvc_px_yuv_to_nhwc_fp16(const uint8_t *yp, int y_stride,
                              const uint8_t *up, const uint8_t *vp,
                              int uv_stride, int nv12,
                              int W, int H, uint16_t *nhwc);

/**
 * @brief NCHW fp16 YUV（3 平面，Y/U/V）→ NV12 平面（u8，饱和到 0..255）。
 *
 * 语义等价 clip(0,1) + ×255 + lrintf + 截断饱和：
 * 越界值直接由整数饱和覆盖（负→0，>255→255），无需单独的 clip 遍。
 */
void mlvc_px_nchw_yuv_fp16_to_nv12_planes(const uint16_t *src, int W, int H,
                                          uint8_t *yp, int y_stride,
                                          uint8_t *uv, int uv_stride);

/**
 * @brief NC1HWC2 fp16（前 3 通道为 Y/U/V）→ NV12 平面（u8，饱和到 0..255）。
 */
void mlvc_px_nc1hwc2_fp16_to_nv12_planes(const uint16_t *src, int W, int H,
                                         int c2,
                                         uint8_t *yp, int y_stride,
                                         uint8_t *uv, int uv_stride);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_PIXEL_H */
