/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session.h
 * @brief rkvc v2 会话：图式编解码管线生命周期与统计。
 */

#ifndef RKVC_SESSION_H
#define RKVC_SESSION_H

#include "rkvc/pipeline.h"
#include "rkvc/port.h"
#include "rkvc/policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 不透明会话句柄。 */
typedef struct rkvc_session rkvc_session;

/**
 * @brief 会话运行统计。
 *
 * `decode_sec` / `rga_sec` / `write_sec` / `postproc_sec` 主要在
 * `FILE_DECODE` bench 路径累积，流式模板可能为 0。
 */
typedef struct {
    rkvc_route_plan route;          /**< 创建时 Router 解析结果 */
    int             running;        /**< 非 0 表示已 start 且未 stop */
    uint64_t        frames_in;      /**< 输入帧/包计数 */
    uint64_t        frames_out;     /**< 输出帧/包计数 */
    uint64_t        frames_dropped; /**< 丢弃帧数 */
    uint64_t        bytes_out;      /**< 输出字节数（码流包或视频帧） */
    double          avg_fps;        /**< 平均帧率（输出侧） */
    double          decode_sec;     /**< 解码阶段累计耗时（秒） */
    double          rga_sec;        /**< RGA 上采样累计耗时（秒） */
    double          write_sec;      /**< NV12 写盘累计耗时（秒） */
    double          postproc_sec;   /**< `rga_sec + write_sec` */
} rkvc_session_stats;

/**
 * @brief 创建会话（不启动管线）。
 *
 * 隐式调用 `rkvc_init()`。`post_upscale_algo == RKVC_UPSCALE_AI_SR` 时
 * 必须提供非空 `post_upscale_rkvc_model_path`。
 *
 * @param desc 管线描述（拷贝入会话）。
 * @param out  输出会话指针。
 * @return `RKVC_OK` 或错误码。
 */
rkvc_err rkvc_session_create(const rkvc_pipeline_desc *desc,
                             rkvc_session **out);

/**
 * @brief 打开编解码节点并标记为运行中。
 *
 * 流式模板须在 push/pull 前调用；`rkvc_session_run_file` 内部会自动 start。
 *
 * @return `RKVC_OK` 或硬件/FFmpeg 初始化错误。
 */
rkvc_err rkvc_session_start(rkvc_session *session);

/**
 * @brief 请求停止并 flush 管线。
 *
 * 可多次调用；未 start 时安全 no-op。
 */
rkvc_err rkvc_session_stop(rkvc_session *session);

/**
 * @brief 获取会话创建时解析的编解码路线。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_session_get_route(const rkvc_session *session,
                                rkvc_route_plan *plan);

/**
 * @brief 按名称获取命名端口。
 *
 * 有效名称：`"capture"`（输入视频）、`"output"`（输出视频或码流）、
 * `"preview"`（预览支路，部分模板）。未知名称返回 NULL。
 *
 * @return 端口指针，或 NULL。
 */
rkvc_port *rkvc_session_port(rkvc_session *session, const char *name);

/**
 * @brief 读取会话统计快照。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_session_get_stats(const rkvc_session *session,
                                rkvc_session_stats *stats);

/**
 * @brief 销毁会话并释放所有节点与端口队列。
 *
 * 若仍在运行，等效于先 stop。NULL 安全。
 */
void rkvc_session_destroy(rkvc_session *session);

/**
 * @brief 文件 / 采集模板：阻塞跑完整条管线。
 *
 * - `FILE_*` / `AV1_STORAGE`：处理 `input_path`/`output_path`
 * - `LIVE_CAPTURE`：从 `capture_device` 拉帧编码到 `output_path`
 *
 * 内部 `start` → 处理 → `stop`。纯端口流式（手动 push/pull）请勿使用。
 *
 * @return `RKVC_OK` 或 I/O / 编解码错误。
 */
rkvc_err rkvc_session_run_file(rkvc_session *session);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_SESSION_H */
