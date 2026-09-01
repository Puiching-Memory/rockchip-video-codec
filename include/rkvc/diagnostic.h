/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file diagnostic.h
 * @brief 错误与诊断链：把同一诊断输出为人类文本或 JSON。
 */

#ifndef RKVC_DIAGNOSTIC_H
#define RKVC_DIAGNOSTIC_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 诊断单元：一次失败/淘汰的可解释记录。
 *
 * 图规划器与后端在“为何选中/淘汰某个候选”时逐条追加诊断，
 * 形成可检查的链（类似错误堆栈），供 CLI 按文本或 JSON 输出。
 */
typedef struct rkvc_diag {
    rkvc_status status;          /**< 关联状态码 */
    int         stage;           /**< 阶段编码（规划/打开/处理/刷新） */
    const char *subject;         /**< 主体（后端 id / 模型 id / 端口名） */
    const char *reason;          /**< 静态人类可读原因，不包含不可信原始串 */
    const struct rkvc_diag *next; /**< 下一层（更底层）诊断；NULL 终止 */
} rkvc_diag;

/**
 * @brief 把诊断链格式化为一行人类文本（写入 out）。
 * @param diag   链首（可为 NULL = OK）。
 * @param buf    目标缓冲。
 * @param size   buf 字节数。
 */
void rkvc_diag_fmt_text(const rkvc_diag *diag, char *buf, size_t size);

/**
 * @brief 把诊断链格式化为一段 JSON（无换行）。
 * @param diag   链首。
 * @param buf    目标缓冲。
 * @param size   buf 字节数。
 */
void rkvc_diag_fmt_json(const rkvc_diag *diag, char *buf, size_t size);

/**
 * @brief 在诊断链首追加一个节点。
 *
 * 供后端 DSO 在 configure/open/process/flush 失败时记录可解释原因；
 * 宿主侧（规划器/执行器/任务层）同样使用。subject/reason 必须是静态
 * 字符串（不拷贝，不释放）。
 *
 * @param diag   指向链首指针（可为空链）。失败时记为 NOMEM 节点并仍返回原链。
 * @param status 关联状态码。
 * @param stage  阶段编码（1=规划 2=打开 3=处理/刷新）。
 * @param subject 主体（后端 id / 模型 id / 端口名）。
 * @param reason  静态原因。
 */
void rkvc_diag_push(rkvc_diag **diag, rkvc_status status, int stage,
                    const char *subject, const char *reason);

/**
 * @brief 释放诊断链占据的动态内存（由库分配时调用）。
 */
void rkvc_diag_release(rkvc_diag *diag);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_DIAGNOSTIC_H */
