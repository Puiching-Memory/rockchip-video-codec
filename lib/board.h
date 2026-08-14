/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file board.h
 * @brief 板卡 profile 内部定义（lib/ 内部使用，不对外暴露结构细节）。
 */

#ifndef RKVC_INTERNAL_BOARD_H
#define RKVC_INTERNAL_BOARD_H

#include "rkvc/board.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 单块板卡的硬件 profile。
 *
 * 所有板级常量集中于此表，源码其余部分不得再硬编码板级假设。
 * `vpu_*_enc_hw` / `vpu_*_dec_hw` 表示该板 VPU 是否**硬件**支持对应编解码；
 * 软件编码器（如 SVT-AV1）与板卡无关，仍可由运行时探测判定。
 */
typedef struct {
    rkvc_board_id id;       /**< 板卡标识 */
    const char *name;       /**< 稳定名称（`"rk3588"` 等，与 rkvc_board_id_name 一致） */
    const char *soc;        /**< device-tree `<soc>` 片段（如 `"rk3588"`、`"rv1126"`） */

    int max_enc_w;          /**< VPU 硬件编码最大宽度 */
    int max_enc_h;          /**< VPU 硬件编码最大高度 */
    int max_dec_w;          /**< VPU 硬件解码最大宽度（即旧 `caps.max_width` 语义） */
    int max_dec_h;          /**< VPU 硬件解码最大高度 */

    int vpu_h264_enc_hw;    /**< 硬件 H.264 编码 */
    int vpu_hevc_enc_hw;    /**< 硬件 HEVC 编码 */
    int vpu_av1_enc_hw;     /**< 硬件 AV1 编码（无则为 0，软件 SVT-AV1 另算） */
    int vpu_h264_dec_hw;    /**< 硬件 H.264 解码 */
    int vpu_hevc_dec_hw;    /**< 硬件 HEVC 解码 */
    int vpu_av1_dec_hw;     /**< 硬件 AV1 解码 */

    int has_npu;            /**< 是否具备 NPU */
    int npu_tops_x100;      /**< NPU 算力（INT8 TOPS × 100，整数存储） */
    int npu_cores;          /**< NPU 计算核心数（如 RK3588=3、RV1126B=1） */
    int has_rga;            /**< 是否具备 RGA 2D 加速 */
} rkvc_board_profile;

/** @brief 取指定板卡的只读 profile（未知板卡回退到编译期默认）。 */
const rkvc_board_profile *rkvc_board_profile_get(rkvc_board_id id);

/** @brief 取当前激活板卡的只读 profile（等价于 get(detect_board())）。 */
const rkvc_board_profile *rkvc_board_profile_active(void);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_INTERNAL_BOARD_H */
