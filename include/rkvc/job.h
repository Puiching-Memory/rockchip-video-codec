/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file job.h
 * @brief 已规划并可 start/wait/cancel 的一次执行。
 *
 * `rkvc_job` 由库对请求完成图规划后创建，持有节点实例与执行线程；
 * 客户端用 `rkvc_job_push`/`rkvc_job_pull` 流式搬运帧，或因文件端点
 * 使用阻塞驱动的 `rkvc_job_run`（自动读入到写出）。
 */

#ifndef RKVC_JOB_H
#define RKVC_JOB_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"
#include "rkvc/frame.h"
#include "rkvc/request.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rkvc_job rkvc_job;

/* ── 作业生命周期 ─────────────────────────────────────────────────── */
/**
 * @brief 根据请求创建并规划一个作业（不启动执行）。
 *
 * @param ctx   上下文（持有后端/模型/设备能力）。
 * @param req   请求（必须已由 `rkvc_request_init` 初始化）。
 * @param diag  可选：输出规划失败的原因链。
 * @param out   输出作业句柄。成功时返回 RKVC_STATUS_OK。
 */
rkvc_status rkvc_job_create(const rkvc_context *ctx,
                            const rkvc_request *req,
                            rkvc_diag **diag,
                            rkvc_job **out);

/**
 * @brief 启动作业执行（打开设备、分配大块内存，必要时拉起线程）。
 * @param diag 可选：输出打开/协商失败原因链。
 */
rkvc_status rkvc_job_start(rkvc_job *job, rkvc_diag **diag);

/**
 * @brief 等待作业完成（阻塞到 EOS 或错误）。
 * @return 完成状态；`RKVC_STATUS_CANCELED` 表示已取消。
 */
rkvc_status rkvc_job_wait(rkvc_job *job);

/**
 * @brief 请求取消作业；返回后作业进入取消状态，可安全 wait。
 */
rkvc_status rkvc_job_cancel(rkvc_job *job);

/**
 * @brief 销毁作业并按逆序释放全部节点与缓冲。
 * @param job 可为 NULL（无操作）。
 */
void rkvc_job_destroy(rkvc_job *job);

/* ── 流式推/拉（零拷贝路径） ─────────────────────────────────────── */
/**
 * @brief 向作业输入端点推入一帧（非阻塞；队列满返回 RKVC_STATUS_AGAIN）。
 */
rkvc_status rkvc_job_push(rkvc_job *job, rkvc_frame *frame);

/**
 * @brief 从作业输出端点拉取一帧（阻塞至有帧或流结束）。
 * @return RKVC_STATUS_OK 时帧引用被转移给调用方，须由调用方
 *         `rkvc_frame_release`；RKVC_STATUS_EOF 表示流结束；
 *         RKVC_STATUS_CANCELED 表示被取消。
 */
rkvc_status rkvc_job_pull(rkvc_job *job, rkvc_frame **frame);

/** @brief 通知输入已到流结束（flush 语义）。 */
rkvc_status rkvc_job_push_eos(rkvc_job *job);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_JOB_H */
