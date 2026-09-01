/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file request.h
 * @brief 一次媒体处理请求：只描述输入、输出与质量/时延约束。
 *
 * 请求不包含 `MppCtx`、`AVFrame`、RKNN context 或模型文件布局；
 * 图规划器负责据设备能力与模型注册表选择可执行路径。
 */

#ifndef RKVC_REQUEST_H
#define RKVC_REQUEST_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"
#include "rkvc/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 处理类型 ─────────────────────────────────────────────────────── */
/** @brief 请求的媒体处理类型。 */
typedef enum rkvc_operation {
    RKVC_OPERATION_TRANSCODE = 0, /**< 容器 → 容器 */
    RKVC_OPERATION_DECODE,        /**< 容器 → 原始帧 */
    RKVC_OPERATION_ENCODE,        /**< 原始帧 → 容器 */
    RKVC_OPERATION_UPSCALE,       /**< 单帧/短序列上采样 */
} rkvc_operation;

/* ── 视频编解码族 ─────────────────────────────────────────────────── */
/** @brief 视频编解码族。 */
typedef enum rkvc_codec {
    RKVC_CODEC_AUTO = 0,   /**< 由策略与设备能力决定 */
    RKVC_CODEC_H264,       /**< H.264/AVC */
    RKVC_CODEC_HEVC,       /**< H.265/HEVC */
    RKVC_CODEC_AV1,        /**< AV1 */
} rkvc_codec;

/* ── 策略（软约束，供排序使用） ───────────────────────────────────── */
/** @brief 资源/质量策略（软约束，供图规划器对候选排序）。 */
typedef enum rkvc_policy {
    RKVC_POLICY_REALTIME = 0,  /**< 低时延优先 */
    RKVC_POLICY_BALANCED,      /**< 时延与质量折中 */
    RKVC_POLICY_QUALITY,       /**< 质量优先（可能牺牲时延） */
    RKVC_POLICY_OFFLINE,       /**< 非实时高质量 */
} rkvc_policy;

/* ── 质量/时延约束 ────────────────────────────────────────────────── */
/** @brief 质量/时延约束（编码侧重；均为软约束，尽力满足）。 */
typedef struct rkvc_quality {
    int32_t  bitrate_bps;    /**< 目标码率；<=0 表示由策略决定 */
    int32_t  qp;             /**< 固定 QP；<0 表示自动 */
    double   target_latency_ms; /**< 软时延上限；<=0 表示不约束 */
    uint32_t critical_interval_ms; /**< 硬时延上限；0 = 未设 */
    uint8_t  crop_to_16;     /**< 编解码前按 16 对齐裁剪 */
} rkvc_quality;

/* ── 输入/输出端点 ────────────────────────────────────────────────── */
/** @brief 输入/输出端点类型。 */
typedef enum rkvc_endpoint_kind {
    RKVC_ENDPOINT_FILE = 0,     /**< 本地文件路径 */
    RKVC_ENDPOINT_FRAME_SINK,   /**< 由调用方 push/pull 的帧队列 */
    RKVC_ENDPOINT_STREAM,       /**< 库级 net stream (rkmux/rtsp) */
} rkvc_endpoint_kind;

/** @brief 一个输入或输出端点。 */
typedef struct rkvc_endpoint {
    rkvc_endpoint_kind kind;    /**< 端点类型 */
    const char *uri;            /**< FILE 时有效；STREAM 时为网络地址 */
    rkvc_frame_fmt fmt;         /**< 原始帧端点的期望格式 */
} rkvc_endpoint;

/* ── 请求 ─────────────────────────────────────────────────────────── */
/** @brief 一次媒体处理请求（输入、输出与质量/时延约束）。 */
typedef struct rkvc_request {
    rkvc_header   header;       /**< struct_size/api_version */
    rkvc_operation operation;   /**< 处理类型 */
    rkvc_codec    codec;        /**< 编解码族；TRANSCODE 时表示目标编码 */
    rkvc_policy   policy;       /**< 策略（软约束） */
    rkvc_quality  quality;      /**< 质量/时延约束 */
    rkvc_endpoint input;        /**< 输入端点 */
    rkvc_endpoint output;       /**< 输出端点 */
    uint32_t      width;        /**< 期望输出宽度；0 = 跟随源 */
    uint32_t      height;       /**< 期望输出高度；0 = 跟随源 */
    const char   *model_id;     /**< 可选：稳定模型 ID 覆盖自动选择；NULL=自动 */
} rkvc_request;

/**
 * @brief 初始化请求结构（填充头部 + 零初始化）。
 * @param req  用户提供的请求缓冲。
 * @param size sizeof(req)。
 */
void rkvc_request_init(rkvc_request *req, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_REQUEST_H */
