/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#ifndef RKVC_SR_PHASE_H
#define RKVC_SR_PHASE_H

#include <stddef.h>
#include <stdint.h>

/* Puiching-Memory/rknn-super-resolution 单输入部署 core 的固定契约：
 * YCbCr444 -> PixelUnshuffle(2) -> 12 通道；108 通道残差 -> PixelShuffle(6)。 */
#define RKVC_SR_PHASE_INPUT_FACTOR  2
#define RKVC_SR_PHASE_OUTPUT_FACTOR 6
#define RKVC_SR_PHASE_INPUT_CHANNELS  12
#define RKVC_SR_PHASE_OUTPUT_CHANNELS 108

/**
 * 将 NV12 打包成 PixelUnshuffle 兼容的 uint8 phase tensor。
 * NV12 的 4:2:0 色度用双线性扩展到 4:4:4；输出大小为 12*(h/2)*(w/2)。
 * 宿主字节序为 NHWC（每 core 像素 12 通道交错）：rknn 驱动把输入属性
 * dims 直接当作宿主缓冲线性布局，平面 NCHW 喂入会被转置而推理失真。
 */
int rkvc_sr_phase_pack_nv12(const uint8_t *y_plane, int y_stride,
                            const uint8_t *uv_plane, int uv_stride,
                            int width, int height, uint8_t *phases,
                            size_t phases_size);

/**
 * 将 phase residual 做 PixelShuffle(6)，叠加到 bicubic NV12 基线。
 * residual 宿主字节序为 NCHW 平面（rknn 输出属性 dims 为 1x108xHxW，
 * 与输入属性的 NHWC 交错不对称）。Y 逐像素叠加；Cb/Cr 的 4:4:4
 * 残差经 2x2 平均后叠加到 NV12 色度面。
 */
int rkvc_sr_phase_add_residual_nv12(const float *residual,
                                    int residual_w, int residual_h,
                                    uint8_t *y_plane, int y_stride,
                                    uint8_t *uv_plane, int uv_stride,
                                    int width, int height);

#endif /* RKVC_SR_PHASE_H */
