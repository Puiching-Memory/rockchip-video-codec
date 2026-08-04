/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file reconfig.h
 * @brief Session 运行中热切换（码率 / GOP / 强制 IDR）。
 *
 * - **MPP（H.264/HEVC）**：下一帧编码前把 `bit_rate`/`gop_size` 推到
 *   `AVCodecContext`，由 `rkmppenc` 经 `MPP_ENC_SET_CFG` 生效；`request_idr`
 *   通过下一帧 `pict_type=I` 触发 `MPP_ENC_SET_IDR_FRAME`。
 * - **SVT-AV1**：仅更新 `pipeline_desc` 记录；运行中改参需重启编码器
 *   （返回 `RKVC_ERR_INVALID` 若要求立即生效且无 MPP）。
 * - **分辨率 / profile**：不在本 API；需重建 Session（mux/SPS 绑定）。
 *
 * 应用层带宽自适应应调用本 API；策略状态机不进 SDK。
 */

#ifndef RKVC_RECONFIG_H
#define RKVC_RECONFIG_H

#include "rkvc/types.h"
#include "rkvc/session.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 运行中改目标码率（bps）。
 *
 * @param bitrate 目标码率，须 >0。
 * @return `RKVC_OK`、`RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_session_set_bitrate(rkvc_session *session, int64_t bitrate);

/**
 * @brief 运行中改 GOP（帧数）。
 *
 * @param gop_size 须 ≥1。
 */
rkvc_err rkvc_session_set_gop(rkvc_session *session, int gop_size);

/**
 * @brief 请求下一编码帧为 IDR（仅 MPP 路径立即生效）。
 */
rkvc_err rkvc_session_request_idr(rkvc_session *session);

/**
 * @brief 批量热切换描述（未置位字段保持不变）。
 *
 * `flags` 位：
 * - `RKVC_RECONFIG_BITRATE`
 * - `RKVC_RECONFIG_GOP`
 * - `RKVC_RECONFIG_IDR`
 */
#define RKVC_RECONFIG_BITRATE (1u << 0)
#define RKVC_RECONFIG_GOP     (1u << 1)
#define RKVC_RECONFIG_IDR     (1u << 2)

typedef struct {
    unsigned flags;   /**< 哪些字段生效 */
    int64_t  bitrate; /**< 目标码率（bps） */
    int      gop_size;/**< GOP 帧数 */
} rkvc_reconfig_desc;

/**
 * @brief 按 flags 批量应用热切换。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_session_reconfigure(rkvc_session *session,
                                  const rkvc_reconfig_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_RECONFIG_H */
