/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_sr_neon.h
 * @brief NEON 加速的 RKVC SR 量化/反量化（无 NEON 时回退标量）。
 */

#ifndef RKVC_SR_NEON_H
#define RKVC_SR_NEON_H

#include <stddef.h>
#include <stdint.h>

void rkvc_sr_quant_rgb24_nhwc_to_int8(const uint8_t *rgb, int8_t *out,
                                      size_t n, int32_t zp, float scale);

void rkvc_sr_dequant_nchw_int8_to_rgb24(const int8_t *nchw, int w, int h,
                                        int32_t zp, float scale, uint8_t *rgb,
                                        int rgb_stride);

#endif /* RKVC_SR_NEON_H */
