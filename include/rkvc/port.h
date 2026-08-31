/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file port.h
 * @brief Session 命名端口：有界队列 push / pull。
 */

#ifndef RKVC_PORT_H
#define RKVC_PORT_H

#include "rkvc/types.h"
#include "rkvc/buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Session 命名端口（不透明，由 `rkvc_session_port` 获取）。 */
typedef struct rkvc_port rkvc_port;

/**
 * @brief 向端口队列推送一帧/一包。
 *
 * 内部对 `buf` 做 `rkvc_buffer_ref`；调用方可立即 `rkvc_buffer_unref` 自身引用。
 * 队列满时返回 `RKVC_ERR_AGAIN`（深度由 `rkvc_pipeline_desc.queue_depth` 控制）。
 *
 * @param port 有效端口指针。
 * @param buf  视频或码流缓冲。`LIVE_TRANSCODE` 的 `capture` 同时接受两者；
 *             压缩码流须为单个 Annex-B access unit。
 * @return `RKVC_OK`、`RKVC_ERR_AGAIN`（队列满）、`RKVC_ERR_INVALID` 等。
 */
rkvc_err rkvc_port_push(rkvc_port *port, rkvc_buffer *buf);

/**
 * @brief 从端口队列拉取一帧/一包。
 *
 * @param port       有效端口指针。
 * @param buf        输出缓冲区指针（调用方负责 `rkvc_buffer_unref`）。
 * @param timeout_ms 超时毫秒：
 *                   - `0`：非阻塞，队列空则立即 `RKVC_ERR_AGAIN`；
 *                   - `> 0`：等待至多该毫秒数，超时 `RKVC_ERR_AGAIN`；
 *                   - `< 0`：无限阻塞直至有数据。
 * @return `RKVC_OK`、`RKVC_ERR_AGAIN`（空/超时）、`RKVC_ERR_EOF`
 *         （`LIVE_TRANSCODE` 已停止且队列已排空）、`RKVC_ERR_INVALID` 等。
 */
rkvc_err rkvc_port_pull(rkvc_port *port, rkvc_buffer **buf, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_PORT_H */
