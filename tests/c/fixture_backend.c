/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file fixture_backend.c
 * @brief 测试夹具：合法后端 DSO（导出 rkvc_backend_query，ABI 匹配）。
 *
 * 编译为 .so 后供 test_backend_loader 验证加载、握手与注册路径。
 */

#include "graph_internal.h"

/** probe 回调：恒通过（无设备依赖）。 */
static int fixture_probe(const rkvc_device_caps *caps, void *probe_ctx,
                         rkvc_diag **diag) {
    (void)caps; (void)probe_ctx; (void)diag;
    return 0;
}

/** factories 回调：空表。 */
static const rkvc_node_factory *fixture_factories(void *probe_ctx,
                                                  size_t *count) {
    (void)probe_ctx;
    *count = 0;
    return NULL;
}

/** 夹具后端描述符。 */
static rkvc_backend g_fixture = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "fixture-good",
    .capability_flags = RKVC_BACKEND_CAP_RGA,
    .probe = fixture_probe,
    .factories = fixture_factories,
};

const rkvc_backend *rkvc_backend_query(void) { return &g_fixture; }
