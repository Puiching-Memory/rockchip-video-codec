/**
 * @file runtime.h
 * @brief 进程级资源配额（多 Session 并发背压）。
 */

#ifndef RKVC_RUNTIME_H
#define RKVC_RUNTIME_H

#include "rkvc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 全局并发配额。
 *
 * 字段为 0 表示不限制。`rkvc_session_create` 在超限时返回 `RKVC_ERR_AGAIN`。
 */
typedef struct {
    int max_sessions;     /**< 同时存活的 Session 上限 */
    int max_enc_sessions; /**< 带编码器的 Session 上限 */
    int max_npu_sessions; /**< 使用 `rkvc_sr` 的 Session 上限 */
} rkvc_runtime_quota;

/**
 * @brief 设置进程级配额（可在任意时刻调用；立即影响后续 create）。
 * @param quota 配额；NULL 表示恢复默认（全部不限制）。
 */
rkvc_err rkvc_runtime_set_quota(const rkvc_runtime_quota *quota);

/** @brief 读取当前配额与占用。 */
typedef struct {
    rkvc_runtime_quota quota;
    int                sessions;
    int                enc_sessions;
    int                npu_sessions;
} rkvc_runtime_stats;

rkvc_err rkvc_runtime_get_stats(rkvc_runtime_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_RUNTIME_H */
