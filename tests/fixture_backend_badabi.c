/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/* 测试夹具：ABI 不匹配的后端 DSO（必须被加载器淘汰）。 */

#include "graph_internal.h"

static rkvc_backend g_bad = {
    .abi_version = 0xffffu, /* 错误 ABI */
    .id = "fixture-badabi",
};

const rkvc_backend *rkvc_backend_query(void) { return &g_bad; }
