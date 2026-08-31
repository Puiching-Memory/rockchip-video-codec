/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file context.c
 * @brief rkvc_context：设备探测、后端注册表与图规划器。
 *
 * 探测失败只淘汰候选，不使上下文创建失败；设备能力用 `rkvc_probe_device`
 * 查询。候选顺序由注册顺序 + 工厂顺序决定（确定性），不依赖 readdir()。
 */

#include "context_internal.h"
#include "graph_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void probe_soc(char *out, size_t out_size) {
    FILE *fp;
    const char *path = "/proc/device-tree/compatible";
    (void)out_size;
    out[0] = '\0';
    fp = fopen(path, "rb");
    if (!fp)
        return;
    /* compatible 以 NUL 分隔多个字符串；取第一个 rockchip,<soc> */
    {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        size_t i;
        fclose(fp);
        buf[n] = '\0';
        for (i = 0; i + 1 < n; ++i) {
            if (buf[i] == '\0')
                continue;
            if (strncmp(&buf[i], "rockchip,", 9) == 0) {
                size_t j = i + 9;
                while (j < n && buf[j] != '\0' &&
                       (j - (i + 9)) < out_size - 1) {
                    out[j - (i + 9)] = buf[j];
                    ++j;
                }
                out[j - (i + 9)] = '\0';
                break;
            }
            i += strlen(&buf[i]);
        }
    }
}

static void probe_device(rkvc_device_caps *caps) {
    memset(caps, 0, sizeof(*caps));
    probe_soc(caps->soc, sizeof(caps->soc));
    /* NPU 无 probe 时不置位，缺省 0；能力位由后端在 device 上实际探测。 */
    caps->npu_cores = 0;
}

/* ── 上下文 ───────────────────────────────────────────────────────── */

rkvc_status rkvc_context_create(const rkvc_context_options *opts,
                                rkvc_context **out) {
    rkvc_context *c;
    if (!out)
        return RKVC_STATUS_INVALID;
    c = rkvc_g_calloc(1, sizeof(*c));
    if (!c)
        return RKVC_STATUS_NOMEM;
    c->header.struct_size = sizeof(*c);
    c->header.api_version = RKVC_ABI_VERSION;
    c->thread_model = opts ? opts->thread_model : RKVC_THREAD_MODEL_DEFAULT;
    if (opts)
        c->paths = opts->paths; /* 指针借用：调用方须保持数组存活 */
    probe_device(&c->caps);
    (void)rkvc_model_registry_scan(c); /* 候选失败只淘汰，不失败创建 */
    (void)rkvc_backend_dso_scan(c);
    *out = c;
    return RKVC_STATUS_OK;
}

void rkvc_context_destroy(rkvc_context *ctx) {
    if (!ctx)
        return;
    rkvc_backend_dso_close_all(ctx);
    rkvc_g_free(ctx->models);
    rkvc_g_free(ctx);
}

/* ── 探测查询 ─────────────────────────────────────────────────────── */

rkvc_status rkvc_probe_device(const rkvc_context *ctx, rkvc_device_caps *caps) {
    if (!ctx || !caps)
        return RKVC_STATUS_INVALID;
    *caps = ctx->caps;
    return RKVC_STATUS_OK;
}

size_t rkvc_backend_count(const rkvc_context *ctx) {
    return ctx ? ctx->backend_count : 0;
}

const char *rkvc_backend_id(const rkvc_context *ctx, size_t idx) {
    if (!ctx || idx >= ctx->backend_count)
        return NULL;
    return ctx->backends[idx]->id;
}

/* ── 注册表 ───────────────────────────────────────────────────────── */

rkvc_status rkvc_registry_add_backend(rkvc_context *ctx,
                                      const rkvc_backend *be) {
    if (!ctx || !be || !be->id)
        return RKVC_STATUS_INVALID;
    if (ctx->backend_count >= RKVC_MAX_BACKENDS)
        return RKVC_STATUS_NOMEM;
    /* 校验 ABI 握手 */
    if (be->abi_version != RKVC_ABI_VERSION)
        return RKVC_STATUS_FORMAT;
    ctx->backends[ctx->backend_count++] = be;
    return RKVC_STATUS_OK;
}

const rkvc_backend *rkvc_registry_backend(const rkvc_context *ctx, size_t idx) {
    if (!ctx || idx >= ctx->backend_count)
        return NULL;
    return ctx->backends[idx];
}

const rkvc_node_factory *rkvc_registry_find_factory(const rkvc_context *ctx,
                                                    const char *id) {
    size_t b, f;
    if (!ctx || !id)
        return NULL;
    for (b = 0; b < ctx->backend_count; ++b) {
        size_t count = 0;
        const rkvc_node_factory *fs = ctx->backends[b]->factories
            ? ctx->backends[b]->factories(ctx->backends[b]->probe_ctx, &count)
            : NULL;
        for (f = 0; f < count; ++f)
            if (fs[f].id && strcmp(fs[f].id, id) == 0)
                return &fs[f];
    }
    return NULL;
}

/* ── 规划器 ───────────────────────────────────────────────────────── */

int rkvc_plan_build(const rkvc_context *ctx, const rkvc_request *req,
                    const rkvc_device_caps *caps, rkvc_plan *plan,
                    rkvc_diag **diag) {
    size_t b, total = 0;

    if (!ctx || !req || !plan)
        return (int)RKVC_STATUS_INVALID;

    /* 第一遍：统计候选数（硬过滤），保证确定性顺序由注册+工厂顺序决定 */
    for (b = 0; b < ctx->backend_count; ++b) {
        size_t count = 0;
        const rkvc_node_factory *fs =
            ctx->backends[b]->factories(ctx->backends[b]->probe_ctx, &count);
        size_t f;
        for (f = 0; f < count; ++f)
            if (fs[f].matches(req->operation, req->codec, caps))
                total++;
    }
    if (total == 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NOT_FOUND, 2,
                           "planner", "no candidate path for request");
        return (int)RKVC_STATUS_NOT_FOUND;
    }

    plan->steps = rkvc_g_calloc(total, sizeof(*plan->steps));
    if (!plan->steps)
        return (int)RKVC_STATUS_NOMEM;
    plan->step_count = 0;

    /* 第二遍：按注册顺序 + 工厂顺序填入（确定性） */
    for (b = 0; b < ctx->backend_count; ++b) {
        size_t count = 0;
        const rkvc_node_factory *fs =
            ctx->backends[b]->factories(ctx->backends[b]->probe_ctx, &count);
        size_t f;
        for (f = 0; f < count; ++f) {
            if (!fs[f].matches(req->operation, req->codec, caps))
                continue;
            plan->steps[plan->step_count].factory = &fs[f];
            plan->steps[plan->step_count].request = *req;
            plan->step_count++;
        }
    }
    return 0;
}

void rkvc_plan_release(rkvc_plan *plan) {
    if (!plan)
        return;
    rkvc_g_free(plan->steps);
    plan->steps = NULL;
    plan->step_count = 0;
}
