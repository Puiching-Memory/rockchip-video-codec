/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file builtin_backends.c
 * @brief 内建后端注册点。
 *
 * fileio（文件 source/sink）随核心库始终注册，是文件端点管线的基座。
 * 硬件后端（MPP/RGA/RKNN）经后端 DSO 发现装载；单测可在自身 TU 提供
 * 本入口的强定义注册夹具后端。
 */

#include "context_internal.h"

void rkvc_backend_register_builtins(rkvc_context *ctx) {
    rkvc_fileio_backend_register(ctx);
}
