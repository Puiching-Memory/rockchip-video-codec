/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "rkvc_sr_phase.h"

#include <math.h>

static uint8_t clip_u8(float value)
{
    /* NaN 的有序比较均为假；用否定比较把 NaN 确定性地钳到 0。 */
    if (!(value > 0.0f))
        return 0;
    if (value >= 255.0f)
        return 255;
    return (uint8_t)lrintf(value);
}

static uint8_t nv12_channel_at(const uint8_t *y_plane, int y_stride,
                               const uint8_t *uv_plane, int uv_stride,
                               int width, int height, int channel, int y, int x)
{
    if (channel == 0)
        return y_plane[(size_t)y * y_stride + x];

    const int chroma_w = width / 2;
    const int chroma_h = height / 2;
    const int x0 = x / 2;
    const int y0 = y / 2;
    const int x1 = x0 + 1 < chroma_w ? x0 + 1 : x0;
    const int y1 = y0 + 1 < chroma_h ? y0 + 1 : y0;
    const int offset = channel - 1;
    const int a = uv_plane[(size_t)y0 * uv_stride + x0 * 2 + offset];
    if (!(x & 1) && !(y & 1))
        return (uint8_t)a;
    const int b = uv_plane[(size_t)y0 * uv_stride + x1 * 2 + offset];
    const int c = uv_plane[(size_t)y1 * uv_stride + x0 * 2 + offset];
    if (!(y & 1))
        return (uint8_t)((a + b + 1) / 2);
    if (!(x & 1))
        return (uint8_t)((a + c + 1) / 2);
    const int d = uv_plane[(size_t)y1 * uv_stride + x1 * 2 + offset];
    return (uint8_t)((a + b + c + d + 2) / 4);
}

int rkvc_sr_phase_pack_nv12(const uint8_t *y_plane, int y_stride,
                            const uint8_t *uv_plane, int uv_stride,
                            int width, int height, uint8_t *phases,
                            size_t phases_size)
{
    const int factor = RKVC_SR_PHASE_INPUT_FACTOR;
    if (!y_plane || !uv_plane || !phases || width <= 0 || height <= 0 ||
        y_stride < width || uv_stride < width ||
        (width % factor) != 0 || (height % factor) != 0)
        return -1;

    const int core_w = width / factor;
    const int core_h = height / factor;
    const size_t required = (size_t)RKVC_SR_PHASE_INPUT_CHANNELS * core_w * core_h;
    if (phases_size < required)
        return -1;

    /* torch.nn.functional.pixel_unshuffle 的通道序：
     * channel, row-within-block, column-within-block, y, x。
     * 宿主字节序按 NHWC（像素通道交错）排列：rknn 驱动把输入属性
     * dims（如 1x180x320x12）直接当作宿主缓冲的线性布局解释，
     * 平面 NCHW 喂入会被整体转置，推理结果完全失真。 */
    for (int channel = 0; channel < 3; channel++) {
        for (int dy = 0; dy < factor; dy++) {
            for (int dx = 0; dx < factor; dx++) {
                const int packed_c = channel * factor * factor + dy * factor + dx;
                for (int cy = 0; cy < core_h; cy++) {
                    const int sy = cy * factor + dy;
                    for (int cx = 0; cx < core_w; cx++) {
                        const int sx = cx * factor + dx;
                        const size_t off = ((size_t)cy * core_w + cx) *
                            RKVC_SR_PHASE_INPUT_CHANNELS + packed_c;
                        phases[off] =
                            nv12_channel_at(y_plane, y_stride, uv_plane, uv_stride,
                                            width, height, channel, sy, sx);
                    }
                }
            }
        }
    }
    return 0;
}

/* 逻辑索引（channel, dy, dx, cy, cx）与 shuffled_at 等价。
 * 残差宿主字节序为 NCHW 平面：rknn 输出的属性 dims 是 1x108xHxW，
 * 驱动按平面主序交付（与输入属性 1xHxWx12 的 NHWC 交错并不对称），
 * 因此偏移 = packed_c * plane + cy * residual_w + cx。 */
static inline size_t residual_off(size_t plane, int residual_w,
                                  int channel, int dy, int dx,
                                  int cy, int cx)
{
    const int factor = RKVC_SR_PHASE_OUTPUT_FACTOR;
    const int packed_c = channel * factor * factor + dy * factor + dx;
    return (size_t)packed_c * plane + (size_t)cy * residual_w + cx;
}

int rkvc_sr_phase_add_residual_nv12(const float *residual,
                                    int residual_w, int residual_h,
                                    uint8_t *y_plane, int y_stride,
                                    uint8_t *uv_plane, int uv_stride,
                                    int width, int height)
{
    const int factor = RKVC_SR_PHASE_OUTPUT_FACTOR;
    const size_t plane = (size_t)residual_w * residual_h;
    if (!residual || !y_plane || !uv_plane || residual_w <= 0 || residual_h <= 0 ||
        width != residual_w * factor || height != residual_h * factor ||
        (width & 1) || (height & 1) || y_stride < width || uv_stride < width)
        return -1;

    /* Y：逐 core 块累加 6×6 残差。
     * 循环序特意把 cx 放到最内层：残差按平面主序存放，cx 连续即单流
     * 顺序读（硬件预取有效）；若把 dy/dx 放内层会同时活跃 36（Y）+
     * 72（UV）条平面流，超出 A72 预取流与 L1 dTLB 容量，退化为延迟
     * 受限（实测 65ms/帧）。此时散写目标仅为当前 core 行的 6×1920
     * = 11.5KB，全帧始终命中 L1，不产生额外内存流量。 */
    for (int cy = 0; cy < residual_h; cy++) {
        for (int dy = 0; dy < factor; dy++) {
            uint8_t *dst = y_plane + (size_t)(cy * factor + dy) * y_stride;
            for (int dx = 0; dx < factor; dx++) {
                const float *src = residual +
                    residual_off(plane, residual_w, 0, dy, dx, cy, 0);
                for (int cx = 0; cx < residual_w; cx++) {
                    uint8_t *p = dst + (size_t)cx * factor + dx;
                    *p = clip_u8((float)*p + src[cx]);
                }
            }
        }
    }

    /* UV：色度坐标 y=3*cy+ey, x=3*cx+ex（factor/2=3）；原实现取全分辨率
     * 坐标 (2y..2y+1, 2x..2x+1) 的 2×2 残差均值，展开后四个样本全部落在
     * 同一 core 块 (cy, cx) 内，子索引为 (2ey+dy2, 2ex+dx2)。同样 cx 内层。 */
    for (int cy = 0; cy < residual_h; cy++) {
        for (int ey = 0; ey < factor / 2; ey++) {
            uint8_t *dst = uv_plane + (size_t)(cy * (factor / 2) + ey) * uv_stride;
            for (int ex = 0; ex < factor / 2; ex++) {
                for (int channel = 1; channel <= 2; channel++) {
                    const float *s0 = residual + residual_off(plane, residual_w,
                                                              channel, ey * 2,
                                                              ex * 2, cy, 0);
                    const float *s1 = s0 + plane;       /* dx2 = 1 */
                    const float *s2 = s0 + 6 * plane;   /* dy2 = 1 */
                    const float *s3 = s2 + plane;
                    const int offset = ex * 2 + channel - 1;
                    for (int cx = 0; cx < residual_w; cx++) {
                        uint8_t *p = dst + (size_t)cx * (factor / 2) * 2 + offset;
                        const float sum = s0[cx] + s1[cx] + s2[cx] + s3[cx];
                        *p = clip_u8((float)*p + sum * 0.25f);
                    }
                }
            }
        }
    }
    return 0;
}
