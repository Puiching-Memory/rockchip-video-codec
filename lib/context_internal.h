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

/** 最多同时装载的后端数（内建 + DSO）。 */
#define RKVC_MAX_BACKENDS 32
/** 单个后端最多暴露的工厂数（超出按格式错误淘汰）。 */
#define RKVC_MAX_FACTORIES_PER_BACKEND 256
/** 单次规划最多尝试的候选组合数（防组合爆炸）。 */
#define RKVC_MAX_PLAN_FALLBACKS 64
/** 可信模型目录数上界。 */
#define RKVC_MAX_MODEL_DIRS 8
/** 可信后端目录数上界。 */
#define RKVC_MAX_BACKEND_DIRS 8

/**
 * @brief rkvc_context 内部结构（非公共 ABI）。
 *
 * 探测/注册失败只淘汰候选并计数，不使创建失败。
 */
struct rkvc_context {
    atomic_uint         refcount;       /**< 引用计数（job 持有引用） */
    rkvc_header         header;         /**< struct_size/api_version */
    rkvc_thread_model   thread_model;   /**< 线程模型 */
    rkvc_search_paths   paths;          /**< 搜索路径视图（指向 owned_*） */
    char              **owned_backend_dirs; /**< 深拷贝的后端目录 */
    char              **owned_model_dirs;   /**< 深拷贝的模型目录 */
    char               *model_dir_override; /**< 覆盖模型目录（可空） */
    uint32_t            inspect_timeout_ms; /**< 探测超时 */
    uint32_t            log_level;      /**< 日志级别 */
    rkvc_device_caps    caps;           /**< 设备能力快照 */
    const rkvc_backend *backends[RKVC_MAX_BACKENDS]; /**< 已装载后端（内建在前） */
    size_t              backend_count;  /**< backends 元素数 */
    rkvc_rkmodel       *models;       /**< 注册表（头摘要；创建时扫描） */
    size_t              model_count;     /**< 有效模型候选数 */
    size_t              model_skipped; /**< 被过滤/拒绝的候选数 */
    void               *dso_handles[RKVC_MAX_BACKENDS]; /**< 后端 DSO 句柄 */
    size_t              dso_count;       /**< dso_handles 元素数 */
    size_t              backend_skipped; /**< 被过滤/拒绝的后端候选数 */
    char                backend_diag[192]; /**< 最近一条后端淘汰诊断 */
};

/** Job 等长生命周期对象持有 context，防止后端 DSO 被过早卸载。 */
void rkvc_context_retain(const rkvc_context *ctx);
/** 递减引用计数；减到 0 时关闭 DSO、释放注册表与路径。 */
void rkvc_context_release(rkvc_context *ctx);

/** 扫描可信模型目录，填充注册表；单个候选失败只淘汰该候选。 */
rkvc_status rkvc_model_registry_scan(rkvc_context *ctx);

/** 确定性地选出兼容模型；返回指针归 ctx 持有。 */
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
