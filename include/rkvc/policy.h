/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file policy.h
 * @brief Codec 选择与路由策略（H.264 / HEVC / AV1）。
 */

#ifndef RKVC_POLICY_H
#define RKVC_POLICY_H

#include "rkvc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rkvc_pipeline_desc rkvc_pipeline_desc;

/** @brief 目标编解码格式。 */
typedef enum {
    RKVC_CODEC_H264 = 0,        /**< H.264 / AVC */
    RKVC_CODEC_HEVC,            /**< H.265 / HEVC */
    RKVC_CODEC_AV1,             /**< AV1 */
    RKVC_CODEC_MLVC,            /**< 神经视频编解码（RKNN NPU + rANS 熵编码） */
    RKVC_CODEC_AUTO,            /**< 由 `rkvc_policy` 自动选择（默认） */
} rkvc_codec;

/**
 * @brief 端到端质量/延迟策略（Codec Router 输入）。
 *
 * 与 CLI `-p realtime|balanced|quality|offline` 对应。
 */
typedef enum {
    RKVC_POLICY_REALTIME = 0,   /**< H.264 RKMPP，目标 ≥30fps@1080p */
    RKVC_POLICY_BALANCED,       /**< HEVC RKMPP；高帧率 1080p+ 回退 H.264 */
    RKVC_POLICY_QUALITY,        /**< SVT-AV1 preset 11 + av1_rkmpp（近实时） */
    RKVC_POLICY_OFFLINE,        /**< 非实时：SVT-AV1 preset 4 + av1_rkmpp，目标 ≥1fps@1080p */
    RKVC_POLICY_NEURAL,         /**< MLVC 神经视频编解码（RKNN NPU + rANS 熵编码） */
} rkvc_policy;

/** @brief 编码器后端。 */
typedef enum {
    RKVC_ENC_BACKEND_NONE = 0,
    RKVC_ENC_BACKEND_MPP,       /**< FFmpeg `h264_rkmpp` / `hevc_rkmpp` */
    RKVC_ENC_BACKEND_SVT,       /**< SVT-AV1（`svt-av1`） */
    RKVC_ENC_BACKEND_MLVC,      /**< MLVC 神经编解码（RKNN NPU + 纯 C rANS） */
} rkvc_enc_backend;

/** @brief 解码器后端。 */
typedef enum {
    RKVC_DEC_BACKEND_NONE = 0,
    RKVC_DEC_BACKEND_MPP,       /**< FFmpeg `*_rkmpp` 硬解 */
    RKVC_DEC_BACKEND_MLVC,      /**< MLVC 神经解码（RKNN NPU + 纯 C rANS） */
} rkvc_dec_backend;

/**
 * @brief Codec Router 解析结果。
 *
 * 由 `rkvc_route_resolve` 或 `rkvc_session_get_route` 填充。
 */
typedef struct {
    rkvc_codec        codec;        /**< 选定格式 */
    rkvc_enc_backend  enc_backend;  /**< 编码后端 */
    rkvc_dec_backend  dec_backend;  /**< 解码后端 */
    const char       *enc_name;     /**< FFmpeg 编码器名或 `"svt-av1"` */
    const char       *dec_name;     /**< FFmpeg 解码器名，如 `"hevc_rkmpp"` */
    int               svt_preset;   /**< SVT `enc_mode`（如 4–11），非 SVT 时为 0 */
    const char       *reason;       /**< 人类可读选型原因（静态字符串） */
} rkvc_route_plan;

/**
 * @brief 根据 policy / codec / 分辨率选择编解码路线。
 *
 * `desc->codec != RKVC_CODEC_AUTO` 时强制对应路线，忽略 policy 自动规则。
 *
 * @param desc 管线描述（只读）。
 * @param plan 输出路线。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_route_resolve(const rkvc_pipeline_desc *desc,
                            rkvc_route_plan *plan);

/**
 * @brief 将 `rkvc_codec` 转为 CLI 风格短名。
 * @return `"h264"` / `"hevc"` / `"av1"` / `"auto"`。
 */
const char *rkvc_codec_name(rkvc_codec codec);

/**
 * @brief 将 `rkvc_policy` 转为 CLI 风格短名。
 * @return `"realtime"` / `"balanced"` / `"quality"` / `"offline"` / `"unknown"`。
 */
const char *rkvc_policy_name(rkvc_policy policy);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_POLICY_H */
