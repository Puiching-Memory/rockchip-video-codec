/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file model.h
 * @brief 模型注册表：.rkmodel 候选的只读查询（公共 ABI）。
 *
 * 注册表在 context 创建时扫描可信目录中的 `.rkmodel` 容器，只读取有界
 * 头部并完成签名/兼容性过滤；载荷按请求装载。目录名仅用于组织文件，
 * 不参与正确性判断。
 */

#ifndef RKVC_MODEL_H
#define RKVC_MODEL_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 模型签名验证后的信任级别。 */
typedef enum rkvc_model_trust {
    RKVC_MODEL_TRUST_UNSIGNED = 0, /**< 未签名（仅开发模式可用） */
    RKVC_MODEL_TRUST_DEVELOPMENT,  /**< 开发 trust root 签名通过 */
    RKVC_MODEL_TRUST_PRODUCTION,   /**< 生产 trust root 签名通过 */
    RKVC_MODEL_TRUST_UNTRUSTED,    /**< 签名存在但验证失败/无验证后端 */
} rkvc_model_trust;

/** @brief 模型候选的头部摘要（inspect 用；不暴露文件路径）。 */
typedef struct rkvc_model_info {
    char              id[64];          /**< 稳定模型 ID */
    char              family[32];      /**< 模型族 */
    char              role[32];        /**< 角色 */
    char              version[32];     /**< 模型版本 */
    char              rknn_target[32]; /**< RKNN 编译目标（可空） */
    rkvc_model_trust  trust;           /**< 签名验证结果 */
    uint32_t          payload_mask;    /**< 载荷位掩码（1<<kind） */
} rkvc_model_info;

/** @brief 已注册模型候选数量。 */
size_t rkvc_model_count(const rkvc_context *ctx);

/** @brief 读取第 idx 个候选摘要；越界返回 RKVC_STATUS_NOT_FOUND。 */
rkvc_status rkvc_model_info_at(const rkvc_context *ctx, size_t idx,
                               rkvc_model_info *info);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_MODEL_H */
