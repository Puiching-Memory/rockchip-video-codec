/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file ratectl.h
 * @brief MLVC 闭环码率控制（microsoft/mlvc rate_controller.py 的 C 移植）。
 *
 * 结构与默认参数与官方一致：
 * - LeakyBucket：容量 = bitrate × 1s，初始/目标水位 0.1。
 * - RateAllocator：overshoot_tau=0.25s、undershoot_tau=1.0s（I/LTR_RECOVERY
 *   帧 100.0s）、planned_excess_tau=0.5s；分配钳 [0.33×, 2×] nominal 且
 *   不超过桶 90% 头余量；溢出则丢帧（返回 -1）。
 * - 4 个指数 R-Q 模型 bpp=α·exp(-β·q_index)：I / LTR_RECOVERY /
 *   P-after-IDR / P-after-LTR，α 在线自适应（alpha_tau=2），P 模型 β 带
 *   9 次 update 的 ramp；I/LTR_RECOVERY 后重置两个 P 模型。
 * - 帧权重 I=10 / LTR_RECOVERY=6 / P=1，向 1 衰减（tau=2）。
 *
 * q_index 语义：0..63，越大质量越高（与 H.265 QP 反向）。
 * 丢帧哨兵与官方一致为 -1。
 *
 * 数值口径：全程 double；取整处复刻 Python int()（向零截断）与
 * np.round()（half-to-even，用 nearbyint 默认舍入模式）。
 */

#ifndef RKVC_BACKEND_MLVC_RATECTL_H
#define RKVC_BACKEND_MLVC_RATECTL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 丢帧哨兵（官方 Q_INDEX_DROP_SENTINEL）。 */
#define MLVC_RC_Q_DROP (-1)

typedef enum {
    MLVC_RC_FRAME_I = 0,
    MLVC_RC_FRAME_P = 1,
    MLVC_RC_FRAME_LTR_RECOVERY = 2,
} mlvc_rc_frame_type;

typedef struct mlvc_rate_controller mlvc_rate_controller;

/** 创建（bitrate_bps 目标码率；fps 帧率；宽高用于 bpp 换算）。 */
mlvc_rate_controller *mlvc_rc_create(uint32_t width, uint32_t height,
                                     double bitrate_bps, double fps);

void mlvc_rc_free(mlvc_rate_controller *rc);

/** 热改目标码率（保留水位偏差，对齐官方 configure）。 */
void mlvc_rc_configure(mlvc_rate_controller *rc, double bitrate_bps);

/**
 * @brief 求下一帧 q_index。
 * @param presentation_time 帧展示时刻（秒，单调递增）。
 * @param frame_type 帧型。
 * @param reserved_overhead_bits 帧头开销预留（从分配中扣除）。
 * @return [0,63] q_index；MLVC_RC_Q_DROP 表示本帧应丢弃。
 */
int mlvc_rc_solve_q_index(mlvc_rate_controller *rc, double presentation_time,
                          mlvc_rc_frame_type frame_type,
                          int64_t reserved_overhead_bits);

/**
 * @brief 编码完成反馈。
 *
 * 丢帧（solve 返回 MLVC_RC_Q_DROP）时 payload_bits 传 0：仅更新漏桶，
 * 不扰动 R-Q 模型（对齐官方：丢帧不进模型 update）。
 */
void mlvc_rc_update(mlvc_rate_controller *rc, int64_t header_bits,
                    int64_t payload_bits);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_RATECTL_H */
