/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file context.h
 * @brief 库上下文：持有设备探测、后端/模型注册表、搜索路径与线程设置。
 *
 * 所有全局状态迁入 `rkvc_context`；只允许日志后端与不可变进程信息
 * 进程级共享。上下文负责定义可信后端与模型搜索路径。
 */

#ifndef RKVC_CONTEXT_H
#define RKVC_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rkvc_context rkvc_context;

/* ── 后端/模型搜索路径 ────────────────────────────────────────────── */
typedef struct rkvc_search_paths {
    const char **backend_dirs;  /**< 可信后端 DSO 目录列表 */
    size_t       backend_dir_count;
    const char **model_dirs;    /**< 可信模型注册表目录列表 */
    size_t       model_dir_count;
} rkvc_search_paths;

/* ── 上下文选项 ───────────────────────────────────────────────────── */
typedef struct rkvc_context_options {
    rkvc_header        header;
    rkvc_thread_model  thread_model;
    rkvc_search_paths  paths;         /**< 空 = 使用包内 + 系统默认路径 */
    const char        *model_dir_override; /**< 可选：覆盖模型目录；NULL=自动 */
    uint32_t           inspect_timeout_ms; /**< 探测超时；0 = 默认 2000 */
    uint32_t           log_level;
} rkvc_context_options;

/**
 * @brief 初始化上下文选项（填充头部 + 零初始化）。
 */
void rkvc_context_options_init(rkvc_context_options *opts, size_t size);

/**
 * @brief 创建上下文并探测设备、装载后端与模型注册表。
 *
 * 探测失败只淘汰候选，不会让上下文创建失败；设备与后端能力通过
 * `rkvc_probe_*` 查询。若 `paths` 为空，使用包内相对目录与系统数据目录，
 * 永不扫描当前工作目录。
 *
 * @param opts  可为 NULL（使用默认）。
 * @param out   输出上下文句柄。
 */
rkvc_status rkvc_context_create(const rkvc_context_options *opts,
                                rkvc_context **out);

/**
 * @brief 销毁上下文并释放全部注册表与探测资源。
 */
void rkvc_context_destroy(rkvc_context *ctx);

/* ── 探测查询（只读） ─────────────────────────────────────────────── */
typedef struct rkvc_device_caps {
    char   soc[64];            /**< 探测到的 SoC 名称（device-tree compatible） */
    uint32_t npu_cores;        /**< NPU 核心数；0 = 无 */
    uint32_t has_mpp_decoder;  /**< VPU 硬解可用 */
    uint32_t has_mpp_encoder;  /**< VPU 硬编可用 */
    uint32_t has_rga;          /**< RGA 2D 可用 */
    uint32_t has_rknn;         /**< RKNN 运行时可用 */
} rkvc_device_caps;

/** @brief 读取探测到的设备能力。 */
rkvc_status rkvc_probe_device(const rkvc_context *ctx,
                              rkvc_device_caps *caps);

/** @brief 返回已装载后端的数量与稳定 id 列表（供 inspect 使用）。 */
size_t rkvc_backend_count(const rkvc_context *ctx);

/** @brief 返回第 idx 个后端 id 的稳定字符串；越界返回 NULL。 */
const char *rkvc_backend_id(const rkvc_context *ctx, size_t idx);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_CONTEXT_H */
