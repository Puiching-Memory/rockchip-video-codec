/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file context_internal.h
 * @brief rkvc_context 内部结构（非公共 ABI）。
 */

#ifndef RKVC_CONTEXT_INTERNAL_H
#define RKVC_CONTEXT_INTERNAL_H

#include <stdatomic.h>

#include "rkvc/api.h"
#include "graph_internal.h"
#include "rkmodel.h"

#define RKVC_MAX_BACKENDS 32
#define RKVC_MAX_FACTORIES_PER_BACKEND 256
#define RKVC_MAX_PLAN_FALLBACKS 64
#define RKVC_MAX_MODEL_DIRS 8
#define RKVC_MAX_BACKEND_DIRS 8

struct rkvc_context {
    atomic_uint         refcount;
    rkvc_header         header;
    rkvc_thread_model   thread_model;
    rkvc_search_paths   paths;
    char              **owned_backend_dirs;
    char              **owned_model_dirs;
    char               *model_dir_override;
    uint32_t            inspect_timeout_ms;
    uint32_t            log_level;
    rkvc_device_caps    caps;
    const rkvc_backend *backends[RKVC_MAX_BACKENDS];
    size_t              backend_count;
    rkvc_rkmodel       *models;       /**< 注册表（头摘要；创建时扫描） */
    size_t              model_count;
    size_t              model_skipped; /**< 被过滤/拒绝的候选数 */
    void               *dso_handles[RKVC_MAX_BACKENDS]; /**< 后端 DSO 句柄 */
    size_t              dso_count;
    size_t              backend_skipped; /**< 被过滤/拒绝的后端候选数 */
    char                backend_diag[192]; /**< 最近一条后端淘汰诊断 */
};

/** Job 等长生命周期对象持有 context，防止后端 DSO 被过早卸载。 */
void rkvc_context_retain(const rkvc_context *ctx);
void rkvc_context_release(rkvc_context *ctx);

/** 扫描可信模型目录，填充注册表；单个候选失败只淘汰该候选。 */
rkvc_status rkvc_model_registry_scan(rkvc_context *ctx);

/** Select one compatible model deterministically; returned pointer is owned by ctx. */
const rkvc_rkmodel *rkvc_model_registry_select(const rkvc_context *ctx,
                                               const rkvc_request *req,
                                               rkvc_diag **diag);

/** 注册内建后端（默认空表；硬件后端构建时替换实现）。 */
void rkvc_backend_register_builtins(rkvc_context *ctx);

/** 注册内建 fileio 后端（文件 source/sink；node_fileio.c 提供）。 */
void rkvc_fileio_backend_register(rkvc_context *ctx);

/** 扫描可信后端目录并 dlopen 加载；单个候选失败只淘汰该候选。 */
rkvc_status rkvc_backend_dso_scan(rkvc_context *ctx);

/** 关闭全部后端 DSO 句柄（context 销毁时调用）。 */
void rkvc_backend_dso_close_all(rkvc_context *ctx);

#endif /* RKVC_CONTEXT_INTERNAL_H */
