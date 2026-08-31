/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file api.h
 * @brief rkvc 0.4 公共 ABI —— 五个核心概念的聚合入口。
 *
 * 0.4 公共 API 只暴露五类概念：context、request、job、frame、diagnostic。
 * 本头文件不包含任何 FFmpeg / MPP / RGA / RKNN 类型；后端与模型在内部或
 * 后端 DSO 中实现，通过版本化接口进入图内核。
 *
 * 约束：
 *  - 所有公开结构以 `rkvc_header`（struct_size + api_version）开头。
 *  - 所有句柄 opaque，不暴露内部节点、后端或模型路径。
 *  - 库内线程不回调用户代码时持有内部锁；回调线程语义见各 API 文档。
 */

#ifndef RKVC_API_H
#define RKVC_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── ABI 版本 ─────────────────────────────────────────────────────── */

#define RKVC_ABI_VERSION_MAJOR 0
#define RKVC_ABI_VERSION_MINOR 4
#define RKVC_ABI_VERSION_PATCH 0

/** @brief 编译期 ABI 版本号：major<<16 | minor<<8 | patch。 */
#define RKVC_ABI_VERSION \
    ((RKVC_ABI_VERSION_MAJOR << 16) | \
     (RKVC_ABI_VERSION_MINOR << 8) |  \
     (RKVC_ABI_VERSION_PATCH))

/* ── 状态码（0.4 专用，避免与 0.3 的 rkvc_err 冲突） ─────────────── */

/**
 * @brief 库调用状态码。
 *
 * 负值为错误；`RKVC_STATUS_AGAIN`/`RKVC_STATUS_EOF` 在流式 I/O 中为正常
 * 控制流。错误可通过 `rkvc_diagnostic` 携带更细的定位与原因。
 */
typedef enum rkvc_status {
    RKVC_STATUS_OK              = 0,   /**< 成功 */
    RKVC_STATUS_NOMEM           = -1,  /**< 内存分配失败 */
    RKVC_STATUS_INVALID         = -2,  /**< 参数或请求无效 */
    RKVC_STATUS_NOT_FOUND       = -3,  /**< 后端、模型或路径未找到 */
    RKVC_STATUS_IO              = -4,  /**< I/O 错误 */
    RKVC_STATUS_HW              = -5,  /**< 硬件加速初始化失败 */
    RKVC_STATUS_EOF             = -6,  /**< 流结束 */
    RKVC_STATUS_AGAIN           = -7,  /**< 需要更多输入；队列或缓冲区满/空 */
    RKVC_STATUS_FORMAT          = -8,  /**< 输入/输出格式约束不匹配 */
    RKVC_STATUS_NEGOTIATE       = -9,  /**< 图格式协商失败 */
    RKVC_STATUS_PERMISSION      = -10, /**< 设备节点权限不足 */
    RKVC_STATUS_LICENSE         = -11, /**< 产品授权校验失败 */
    RKVC_STATUS_UNLICENSED      = -12, /**< 未找到产品授权 */
    RKVC_STATUS_CANCELED        = -13, /**< 执行被取消 */
    RKVC_STATUS_UNSUPPORTED     = -14, /**< 请求在当前设备/构建上不受支持 */
    RKVC_STATUS_INTERNAL        = -15, /**< 内部错误 */
    RKVC_STATUS_INTEGRITY       = -16, /**< 完整性校验失败（如模型载荷摘要不符） */
} rkvc_status;

/* ── 头部约定 ─────────────────────────────────────────────────────── */

/**
 * @brief 所有公开结构起始的版本化头部。
 *
 * `struct_size` 由调用方填充为其结构体的大小（含头部），以便库在结构体
 * 后续扩展时保持向后兼容；`api_version` 由调用方填入其编译时的 ABI 版本。
 */
typedef struct rkvc_header {
    size_t    struct_size;   /**< sizeof(caller struct) */
    uint32_t  api_version;   /**< RKVC_ABI_VERSION */
    uint32_t  reserved;      /**< 保留，恒为 0 */
} rkvc_header;

/* ── 资源所有权与线程语义 ────────────────────────────────────────── */

/**
 * @brief 线程安全性约定。
 *
 *  - `rkvc_context`、`rkvc_job` 均可被多个线程并行使用。
 *  - 库内部对共享状态加锁；用户回调（若启用）在库持锁时不会被调用。
 *  - 每当回调被调用时，库不持有内部锁，用户可在回调中安全调用非阻塞 API。
 */
typedef enum rkvc_thread_model {
    RKVC_THREAD_MODEL_DEFAULT = 0,   /**< 库内部线程池 + 自动加锁 */
} rkvc_thread_model;

/* ── 五类概念头的聚合（基础类型已定义，供各头使用） ──────────────── */
#include "rkvc/diagnostic.h"
#include "rkvc/frame.h"
#include "rkvc/request.h"
#include "rkvc/context.h"
#include "rkvc/job.h"
#include "rkvc/model.h"

/**
 * @brief 库版本字符串（与 CMake project(VERSION) 一致）。
 */
const char *rkvc_version(void);

/**
 * @brief ABI 版本号：major<<16 | minor<<8 | patch。
 */
uint32_t rkvc_abi_version(void);

/**
 * @brief 将状态码转为静态描述字符串。
 * @param status 状态码。
 * @return 人类可读描述；未知码返回 "unknown"。
 */
const char *rkvc_status_str(rkvc_status status);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_API_H */
