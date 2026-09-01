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

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_AVAILABLE(type, size, field) \
    ((size) >= offsetof(type, field) + sizeof(((type *)0)->field))

static int header_compatible(const rkvc_header *h, size_t minimum) {
    uint32_t caller_major;
    if (!h || h->struct_size < minimum)
        return 0;
    if (h->api_version == 0)
        return 1;
    caller_major = h->api_version >> 16;
    return caller_major == RKVC_ABI_VERSION_MAJOR;
}

static void free_paths(rkvc_context *ctx) {
    size_t i;
    for (i = 0; i < ctx->paths.backend_dir_count; ++i)
        rkvc_g_free(ctx->owned_backend_dirs[i]);
    for (i = 0; i < ctx->paths.model_dir_count; ++i)
        rkvc_g_free(ctx->owned_model_dirs[i]);
    rkvc_g_free(ctx->owned_backend_dirs);
    rkvc_g_free(ctx->owned_model_dirs);
    rkvc_g_free(ctx->model_dir_override);
}

static rkvc_status copy_dir_list(const char **src, size_t count,
                                 char ***owned_out, const char ***view_out) {
    char **owned;
    size_t i;
    if (!count) {
        *owned_out = NULL;
        *view_out = NULL;
        return RKVC_STATUS_OK;
    }
    if (!src)
        return RKVC_STATUS_INVALID;
    owned = rkvc_g_calloc(count, sizeof(*owned));
    if (!owned)
        return RKVC_STATUS_NOMEM;
    for (i = 0; i < count; ++i) {
        size_t len;
        if (!src[i] || !src[i][0])
            goto invalid;
        len = strlen(src[i]);
        owned[i] = rkvc_g_calloc(len + 1, 1);
        if (!owned[i])
            goto nomem;
        memcpy(owned[i], src[i], len + 1);
    }
    *owned_out = owned;
    *view_out = (const char **)owned;
    return RKVC_STATUS_OK;
invalid:
    for (i = 0; i < count; ++i) rkvc_g_free(owned[i]);
    rkvc_g_free(owned);
    return RKVC_STATUS_INVALID;
nomem:
    for (i = 0; i < count; ++i) rkvc_g_free(owned[i]);
    rkvc_g_free(owned);
    return RKVC_STATUS_NOMEM;
}

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
    size_t opt_size = opts ? opts->header.struct_size : 0;
    rkvc_status st;
    if (!out)
        return RKVC_STATUS_INVALID;
    *out = NULL;
    if (opts && !header_compatible(&opts->header,
            offsetof(rkvc_context_options, thread_model) +
            sizeof(opts->thread_model)))
        return RKVC_STATUS_INVALID;
    c = rkvc_g_calloc(1, sizeof(*c));
    if (!c)
        return RKVC_STATUS_NOMEM;
    atomic_init(&c->refcount, 1);
    c->header.struct_size = sizeof(*c);
    c->header.api_version = RKVC_ABI_VERSION;
    c->thread_model = opts ? opts->thread_model : RKVC_THREAD_MODEL_DEFAULT;
    c->inspect_timeout_ms = 2000;
    if (opts && FIELD_AVAILABLE(rkvc_context_options, opt_size, paths)) {
        if (opts->paths.backend_dir_count > RKVC_MAX_BACKEND_DIRS ||
            opts->paths.model_dir_count > RKVC_MAX_MODEL_DIRS) {
            rkvc_g_free(c);
            return RKVC_STATUS_INVALID;
        }
        st = copy_dir_list(opts->paths.backend_dirs,
                           opts->paths.backend_dir_count,
                           &c->owned_backend_dirs,
                           &c->paths.backend_dirs);
        if (st != RKVC_STATUS_OK) { rkvc_g_free(c); return st; }
        c->paths.backend_dir_count = opts->paths.backend_dir_count;
        st = copy_dir_list(opts->paths.model_dirs,
                           opts->paths.model_dir_count,
                           &c->owned_model_dirs,
                           &c->paths.model_dirs);
        if (st != RKVC_STATUS_OK) { free_paths(c); rkvc_g_free(c); return st; }
        c->paths.model_dir_count = opts->paths.model_dir_count;
    }
    if (opts && FIELD_AVAILABLE(rkvc_context_options, opt_size,
                                model_dir_override) &&
        opts->model_dir_override && opts->model_dir_override[0]) {
        size_t len = strlen(opts->model_dir_override);
        c->model_dir_override = rkvc_g_calloc(len + 1, 1);
        if (!c->model_dir_override) { free_paths(c); rkvc_g_free(c); return RKVC_STATUS_NOMEM; }
        memcpy(c->model_dir_override, opts->model_dir_override, len + 1);
    }
    if (opts && FIELD_AVAILABLE(rkvc_context_options, opt_size,
                                inspect_timeout_ms) && opts->inspect_timeout_ms)
        c->inspect_timeout_ms = opts->inspect_timeout_ms;
    if (opts && FIELD_AVAILABLE(rkvc_context_options, opt_size, log_level))
        c->log_level = opts->log_level;
    probe_device(&c->caps);
    (void)rkvc_model_registry_scan(c); /* 候选失败只淘汰，不失败创建 */
    (void)rkvc_backend_dso_scan(c);
    *out = c;
    return RKVC_STATUS_OK;
}

void rkvc_context_retain(const rkvc_context *ctx) {
    if (ctx)
        atomic_fetch_add_explicit(&((rkvc_context *)ctx)->refcount, 1,
                                  memory_order_relaxed);
}

void rkvc_context_release(rkvc_context *ctx) {
    if (!ctx)
        return;
    if (atomic_fetch_sub_explicit(&ctx->refcount, 1,
                                  memory_order_acq_rel) != 1)
        return;
    rkvc_backend_dso_close_all(ctx);
    rkvc_g_free(ctx->models);
    free_paths(ctx);
    rkvc_g_free(ctx);
}

void rkvc_context_destroy(rkvc_context *ctx) {
    rkvc_context_release(ctx);
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

static rkvc_status validate_backend_factories(const rkvc_backend *be) {
    const rkvc_node_factory *factories;
    size_t count = 0, i;

    if (!be->factories)
        return RKVC_STATUS_OK;
    factories = be->factories(be->probe_ctx, &count);
    if (count > RKVC_MAX_FACTORIES_PER_BACKEND || (count && !factories))
        return RKVC_STATUS_FORMAT;
    for (i = 0; i < count; ++i) {
        const rkvc_node_factory *f = &factories[i];
        if (!f->id || !f->id[0] || !f->backend_id ||
            strcmp(f->backend_id, be->id) != 0 ||
            f->stage < RKVC_NODE_STAGE_SOURCE ||
            f->stage > RKVC_NODE_STAGE_SINK ||
            !f->matches || !f->create)
            return RKVC_STATUS_FORMAT;
    }
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_registry_add_backend(rkvc_context *ctx,
                                      const rkvc_backend *be) {
    rkvc_diag *diag = NULL;
    rkvc_status st;
    size_t i;
    if (!ctx || !be || !be->id || !be->id[0])
        return RKVC_STATUS_INVALID;
    if (ctx->backend_count >= RKVC_MAX_BACKENDS)
        return RKVC_STATUS_NOMEM;
    /* 校验 ABI 握手 */
    if (be->abi_version != RKVC_ABI_VERSION)
        return RKVC_STATUS_FORMAT;
    for (i = 0; i < ctx->backend_count; ++i)
        if (strcmp(ctx->backends[i]->id, be->id) == 0)
            return RKVC_STATUS_FORMAT;
    if (be->probe && be->probe(&ctx->caps, be->probe_ctx, &diag) != 0) {
        rkvc_diag_release(diag);
        return RKVC_STATUS_HW;
    }
    rkvc_diag_release(diag);
    st = validate_backend_factories(be);
    if (st != RKVC_STATUS_OK)
        return st;
    ctx->backends[ctx->backend_count++] = be;
    if (be->capability_flags & RKVC_BACKEND_CAP_MPP_DECODE)
        ctx->caps.has_mpp_decoder = 1;
    if (be->capability_flags & RKVC_BACKEND_CAP_MPP_ENCODE)
        ctx->caps.has_mpp_encoder = 1;
    if (be->capability_flags & RKVC_BACKEND_CAP_RGA)
        ctx->caps.has_rga = 1;
    if (be->capability_flags & RKVC_BACKEND_CAP_RKNN) {
        ctx->caps.has_rknn = 1;
        if (!ctx->caps.npu_cores)
            ctx->caps.npu_cores = 1;
    }
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

struct ranked_factory {
    const rkvc_node_factory *factory;
    int score;
};

static int ranked_factory_cmp(const void *ap, const void *bp) {
    const struct ranked_factory *a = ap;
    const struct ranked_factory *b = bp;
    int rc;
    if (a->score != b->score)
        return a->score > b->score ? -1 : 1;
    rc = strcmp(a->factory->backend_id, b->factory->backend_id);
    if (rc != 0)
        return rc;
    return strcmp(a->factory->id, b->factory->id);
}

int rkvc_plan_build(const rkvc_context *ctx, const rkvc_request *req,
                    const rkvc_device_caps *caps, rkvc_plan *plan,
                    rkvc_diag **diag) {
    rkvc_node_stage required[5];
    size_t required_count = 0, i;

    if (!ctx || !req || !plan)
        return (int)RKVC_STATUS_INVALID;

    /* 文件端点（带 uri）由内建 source/sink 节点承载；流式端点走 push/pull。
     * job 层已校验 FILE 端点必须携带 uri；此处对空 uri 宽容以便规划器
     * 可独立单测。 */
    if (req->input.kind == RKVC_ENDPOINT_FILE && req->input.uri)
        required[required_count++] = RKVC_NODE_STAGE_SOURCE;

    switch (req->operation) {
    case RKVC_OPERATION_ENCODE:
        required[required_count++] = RKVC_NODE_STAGE_ENCODE;
        break;
    case RKVC_OPERATION_DECODE:
        required[required_count++] = RKVC_NODE_STAGE_DECODE;
        break;
    case RKVC_OPERATION_UPSCALE:
        required[required_count++] = RKVC_NODE_STAGE_TRANSFORM;
        break;
    case RKVC_OPERATION_TRANSCODE:
        required[required_count++] = RKVC_NODE_STAGE_DECODE;
        /* A transform is optional and selected only when the request asks for
         * a size change or a matching transform factory is explicitly forced
         * by model_id. */
        if (req->width || req->height || req->model_id)
            required[required_count++] = RKVC_NODE_STAGE_TRANSFORM;
        required[required_count++] = RKVC_NODE_STAGE_ENCODE;
        break;
    default:
        return (int)RKVC_STATUS_INVALID;
    }

    if (req->output.kind == RKVC_ENDPOINT_FILE && req->output.uri)
        required[required_count++] = RKVC_NODE_STAGE_SINK;

    plan->steps = rkvc_g_calloc(required_count, sizeof(*plan->steps));
    if (!plan->steps)
        return (int)RKVC_STATUS_NOMEM;
    plan->step_count = 0;

    for (i = 0; i < required_count; ++i) {
        struct ranked_factory *ranked;
        size_t ranked_count = 0, capacity = 0, b;

        for (b = 0; b < ctx->backend_count; ++b) {
            size_t count = 0;
            const rkvc_backend *be = ctx->backends[b];
            if (be->factories)
                (void)be->factories(be->probe_ctx, &count);
            if (count > RKVC_MAX_FACTORIES_PER_BACKEND ||
                capacity > SIZE_MAX - count) {
                rkvc_plan_release(plan);
                return (int)RKVC_STATUS_FORMAT;
            }
            capacity += count;
        }
        ranked = rkvc_g_calloc(capacity ? capacity : 1, sizeof(*ranked));
        if (!ranked) {
            rkvc_plan_release(plan);
            return (int)RKVC_STATUS_NOMEM;
        }
        for (b = 0; b < ctx->backend_count; ++b) {
            size_t count = 0, f;
            const rkvc_backend *be = ctx->backends[b];
            const rkvc_node_factory *fs = be->factories
                ? be->factories(be->probe_ctx, &count) : NULL;
            if (count > RKVC_MAX_FACTORIES_PER_BACKEND || (count && !fs)) {
                rkvc_g_free(ranked);
                rkvc_plan_release(plan);
                return (int)RKVC_STATUS_FORMAT;
            }
            for (f = 0; f < count; ++f) {
                int score, bonus = 0;
                int64_t total;
                if (fs[f].stage != required[i] || !fs[f].matches ||
                    !fs[f].matches(req->operation, req->codec, caps))
                    continue;
                score = fs[f].priority;
                if (fs[f].score)
                    bonus = fs[f].score(req, caps, fs[f].create_ctx);
                total = (int64_t)score + bonus;
                score = total > INT_MAX ? INT_MAX :
                        total < INT_MIN ? INT_MIN : (int)total;
                if (ranked_count == capacity) {
                    rkvc_g_free(ranked);
                    rkvc_plan_release(plan);
                    return (int)RKVC_STATUS_FORMAT;
                }
                ranked[ranked_count].factory = &fs[f];
                ranked[ranked_count].score = score;
                ranked_count++;
            }
        }
        if (!ranked_count) {
            rkvc_g_free(ranked);
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_NOT_FOUND, 2,
                               "planner", "required stage has no candidate");
            rkvc_plan_release(plan);
            return (int)RKVC_STATUS_NOT_FOUND;
        }
        qsort(ranked, ranked_count, sizeof(*ranked), ranked_factory_cmp);
        plan->steps[plan->step_count].candidates =
            rkvc_g_calloc(ranked_count, sizeof(*plan->steps[0].candidates));
        if (!plan->steps[plan->step_count].candidates) {
            rkvc_g_free(ranked);
            rkvc_plan_release(plan);
            return (int)RKVC_STATUS_NOMEM;
        }
        for (b = 0; b < ranked_count; ++b)
            plan->steps[plan->step_count].candidates[b] = ranked[b].factory;
        plan->steps[plan->step_count].candidate_count = ranked_count;
        plan->steps[plan->step_count].candidate_index = 0;
        plan->steps[plan->step_count].factory = ranked[0].factory;
        plan->steps[plan->step_count].request = *req;
        plan->step_count++;
        rkvc_g_free(ranked);
    }
    return 0;
}

int rkvc_plan_advance(rkvc_plan *plan, size_t failure_step) {
    size_t i, j;
    if (!plan || !plan->step_count)
        return 0;
    if (plan->fallback_count >= RKVC_MAX_PLAN_FALLBACKS)
        return 0;

    /* 首选只替换实际失败的阶段，保留其他已选候选。 */
    if (failure_step < plan->step_count) {
        rkvc_plan_step *step = &plan->steps[failure_step];
        if (step->candidate_index + 1 < step->candidate_count) {
            step->candidate_index++;
            step->factory = step->candidates[step->candidate_index];
            for (j = failure_step + 1; j < plan->step_count; ++j) {
                plan->steps[j].candidate_index = 0;
                plan->steps[j].factory = plan->steps[j].candidates[0];
            }
            plan->fallback_count++;
            return 1;
        }
    }

    /* 失败阶段已耗尽时按稳定的逆序进位寻找下一条组合。 */
    for (i = plan->step_count; i > 0; --i) {
        rkvc_plan_step *step = &plan->steps[i - 1];
        if (i - 1 == failure_step)
            continue;
        if (step->candidate_index + 1 < step->candidate_count) {
            step->candidate_index++;
            step->factory = step->candidates[step->candidate_index];
            for (j = i; j < plan->step_count; ++j) {
                plan->steps[j].candidate_index = 0;
                plan->steps[j].factory = plan->steps[j].candidates[0];
            }
            plan->fallback_count++;
            return 1;
        }
    }
    return 0;
}

void rkvc_plan_release(rkvc_plan *plan) {
    size_t i;
    if (!plan)
        return;
    for (i = 0; i < plan->step_count; ++i)
        rkvc_g_free(plan->steps[i].candidates);
    rkvc_g_free(plan->steps);
    plan->steps = NULL;
    plan->step_count = 0;
    plan->fallback_count = 0;
}
