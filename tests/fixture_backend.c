/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/* 测试夹具：合法后端 DSO（导出 rkvc_backend_query，ABI 匹配）。 */

#include "graph_internal.h"

static int fixture_probe(const rkvc_device_caps *caps, void *probe_ctx,
                         rkvc_diag **diag) {
    (void)caps; (void)probe_ctx; (void)diag;
    return 0;
}

static const rkvc_node_factory *fixture_factories(void *probe_ctx,
                                                  size_t *count) {
    (void)probe_ctx;
    *count = 0;
    return NULL;
}

static rkvc_backend g_fixture = {
    RKVC_ABI_VERSION, "fixture-good", fixture_probe, fixture_factories, NULL,
};

const rkvc_backend *rkvc_backend_query(void) { return &g_fixture; }
