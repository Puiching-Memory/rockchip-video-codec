/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file pipeline.h
 * @brief 管线描述与预置模板。
 */

#ifndef RKVC_PIPELINE_H
#define RKVC_PIPELINE_H

#include "rkvc/types.h"
#include "rkvc/policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 预置管线模板（决定节点拓扑与默认 policy）。 */
typedef enum {
    RKVC_TEMPLATE_FILE_ENCODE = 0,   /**< 原始 NV12 → 容器文件 */
    RKVC_TEMPLATE_FILE_DECODE,       /**< 容器 → 原始 NV12 */
    RKVC_TEMPLATE_FILE_TRANSCODE,    /**< 容器 → 容器（Router 选 codec） */
    RKVC_TEMPLATE_LIVE_CAPTURE,      /**< 低延迟 V4L2 采集 → 编码（需 `capture_device`） */
    RKVC_TEMPLATE_AV1_STORAGE,       /**< 强制 AV1 SVT 存储档 */
} rkvc_pipeline_template;

/**
 * @brief 完整管线配置。
 *
 * 传给 `rkvc_session_create`；可用 `rkvc_pipeline_from_template` 初始化后再覆盖字段。
 */
typedef struct rkvc_pipeline_desc {
    rkvc_pipeline_template template_id; /**< 模板 ID */
    rkvc_policy            policy;      /**< Codec Router 策略 */
    rkvc_codec             codec;       /**< 目标格式，`AUTO` 时由 Router 决定 */

    int            width;           /**< 显示/参考宽度（像素） */
    int            height;          /**< 显示/参考高度（像素） */
    int            fps_num;         /**< 帧率分子 */
    int            fps_den;         /**< 帧率分母 */
    int64_t        bitrate;         /**< 目标码率（bps） */
    rkvc_pix_fmt   pixel_format;    /**< 原始视频像素格式 */
    int            gop_size;        /**< GOP 长度（帧） */
    int            low_latency;     /**< 非 0：低延迟标志（`LIVE_CAPTURE` 模板默认 1） */
    int            queue_depth;     /**< 端口队列深度，默认 3 */
    rkvc_rc_mode   rc_mode;         /**< MPP 码率控制，默认 CBR */
    int            qp_init;         /**< 固定 QP 初值，-1 表示编码器默认 */

    const char    *input_path;      /**< 文件解码/转码输入路径 */
    const char    *output_path;     /**< 文件编码/转码输出路径 */
    /**
     * V4L2 设备路径（`LIVE_CAPTURE` 必填，如 `/dev/video-camera0`）。
     * 特殊值 `"mock"` / `"mock:..."`：合成 NV12，无需真实摄像头（单元测试用）。
     * 采集格式固定协商 NV12；`width`/`height`/`fps_*` 为请求值，实际以驱动为准。
     */
    const char    *capture_device;
    /**
     * `LIVE_CAPTURE` 最大采集帧数；0 表示直到 `rkvc_session_stop` / 错误。
     * 便于无摄像头环境下的短测。
     */
    int            capture_max_frames;
    /** `LIVE_CAPTURE` 单帧等待超时（毫秒），默认 1000。 */
    int            capture_timeout_ms;

    /**
     * 编码前下采样分母（1=全分辨率编码；2=宽高各减半后编码）。
     * `width`/`height` 仍为显示分辨率；解码后可配合 `post_upscale_algo` 还原。
     */
    int            enc_scale_denom;
    rkvc_upscale_algo post_upscale_algo;           /**< 解码后上采样算法 */
    const char    *post_upscale_rkvc_model_path;   /**< `RKVC_UPSCALE_AI_SR` 时必填：RKNN 模型路径 */

    /** SVT-AV1 `level_of_parallelism`（0=按 CPU 核数自动，1–6 手动） */
    int            svt_lp;
    /** SVT-AV1 实时调优（0=关，1=开；适合低延迟，preset 最低 7） */
    int            svt_rtc;

    /**
     * FFmpeg 选项串（`key=val:key2=val2`），作用于 demuxer / decoder / encoder。
     * 例：MPP 编码器 `qp_max=48:qp_min=10`。
     */
    const char    *codec_opts;
} rkvc_pipeline_desc;

/**
 * @brief 返回默认管线描述（1920×1080@30，4Mbps CBR，NV12，`FILE_TRANSCODE`）。
 */
rkvc_pipeline_desc rkvc_pipeline_desc_defaults(void);

/**
 * @brief 按模板初始化管线描述（覆盖模板相关默认 policy / codec）。
 *
 * @param tmpl 模板 ID。
 * @param desc 输出描述（先填默认值再按模板调整）。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`（未知模板）。
 */
rkvc_err rkvc_pipeline_from_template(rkvc_pipeline_template tmpl,
                                     rkvc_pipeline_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_PIPELINE_H */
