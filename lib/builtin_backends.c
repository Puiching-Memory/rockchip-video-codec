/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file builtin_backends.c
 * @brief 内建后端注册点（默认：空表）。
 *
 * 硬件后端（MPP/RGA/RKNN）随 P3 逐节点迁移完成后，由各自的
 * builtin_backends_<name>.c 提供强定义并在编译期替换本文件；核心仅
 * 通过 rkvc_backend_register_builtins() 入口解耦。单测可在自身 TU 提供
 * 强定义注册夹具后端。
 */

#include "context_internal.h"

void rkvc_backend_register_builtins(rkvc_context *ctx) {
    (void)ctx;
}
