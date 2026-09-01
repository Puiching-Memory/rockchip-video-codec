/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file graph.c
 * @brief 通用图内核：节点/端口工具与图生命周期。
 *
 * 图构建分为协商（configure）与实例化（open）两步：协商只确定端口格式，
 * 实例化才打开设备与分配大块内存。任一步失败均按逆序释放已创建对象。
 */

#include "graph_internal.h"
#include "context_internal.h"

#include <string.h>

/* ── 端口/节点工具 ────────────────────────────────────────────────── */

rkvc_port *rkvc_node_get_port(rkvc_node *n, const char *name, int as_input) {
    int i;
    if (!n || !name)
        return NULL;
    if (as_input) {
        for (i = 0; i < n->in_count; ++i)
            if (n->in_ports[i].name && strcmp(n->in_ports[i].name, name) == 0)
                return &n->in_ports[i];
    } else {
        for (i = 0; i < n->out_count; ++i)
            if (n->out_ports[i].name && strcmp(n->out_ports[i].name, name) == 0)
                return &n->out_ports[i];
    }
    return NULL;
}

void rkvc_port_set_desired(rkvc_port *p, const rkvc_frame_spec *spec) {
    if (!p || !spec)
        return;
    p->desired = *spec;
    p->fmt = *spec;
}

/** 合并单边尺寸：0 为通配；两边非 0 且不等则失败。 */
static int merge_dimension(uint32_t a, uint32_t b, uint32_t *out) {
    if (a && b && a != b)
        return -1;
    *out = a ? a : b;
    return 0;
}

/** 解析一条图边。UNKNOWN 格式接受对端完整 spec；
 * 宽高/stride 为 0 时按字段级通配处理。 */
static int negotiate_edge(rkvc_port *out, rkvc_port *in) {
    rkvc_frame_spec resolved;

    if (out->fmt.fmt == RKVC_FRAME_FMT_UNKNOWN &&
        in->fmt.fmt == RKVC_FRAME_FMT_UNKNOWN)
        return 0;
    if (out->fmt.fmt == RKVC_FRAME_FMT_UNKNOWN)
        resolved = in->fmt;
    else if (in->fmt.fmt == RKVC_FRAME_FMT_UNKNOWN)
        resolved = out->fmt;
    else {
        if (out->fmt.fmt != in->fmt.fmt ||
            out->fmt.domain != in->fmt.domain ||
            out->fmt.modifier != in->fmt.modifier)
            return -1;
        resolved = out->fmt;
        if (merge_dimension(out->fmt.width, in->fmt.width,
                            &resolved.width) != 0 ||
            merge_dimension(out->fmt.height, in->fmt.height,
                            &resolved.height) != 0 ||
            merge_dimension(out->fmt.stride, in->fmt.stride,
                            &resolved.stride) != 0 ||
            merge_dimension(out->fmt.ver_stride, in->fmt.ver_stride,
                            &resolved.ver_stride) != 0)
            return -1;
    }
    out->fmt = resolved;
    in->fmt = resolved;
    return 0;
}

int rkvc_node_connect(rkvc_node *a, const char *out_name, rkvc_node *b,
                      const char *in_name) {
    rkvc_port *out = a ? rkvc_node_get_port(a, out_name, 0) : NULL;
    rkvc_port *in  = b ? rkvc_node_get_port(b, in_name, 1) : NULL;
    rkvc_graph *g  = a ? a->graph : NULL;
    rkvc_queue *q;
    if (!out || !in || !g)
        return -2; /* 无效端口或未绑定图 */
    q = rkvc_queue_create(g->queue_capacity ? g->queue_capacity : 4);
    if (!q)
        return -1;
    out->queue = q;
    in->queue = q;
    return 0;
}

int rkvc_node_emit(rkvc_node *n, size_t out_index, rkvc_frame *frame) {
    if (!n || !frame || out_index >= (size_t)n->out_count)
        return -2;
    if (!n->out_ports[out_index].queue || !n->graph)
        return -2;
    return rkvc_queue_push(n->out_ports[out_index].queue, frame,
                           n->graph->exec);
}

/* ── 图生命周期 ───────────────────────────────────────────────────── */

rkvc_graph *rkvc_graph_new(void) {
    rkvc_graph *g = rkvc_g_calloc(1, sizeof(*g));
    if (g) {
        g->queue_capacity = 4;
        g->failure_step = SIZE_MAX;
        g->state = 0;
    }
    return g;
}

void rkvc_graph_set_queue_capacity(rkvc_graph *g, size_t capacity) {
    if (g && capacity)
        g->queue_capacity = capacity;
}

void rkvc_graph_set_context(rkvc_graph *g, const struct rkvc_context *ctx) {
    if (g)
        g->ctx = ctx;
}

/** 丢弃上一条候选路径遗留的模型载荷（图内最多一个 transform 绑定）。 */
static void release_model_payload(rkvc_graph *g) {
    if (g && g->model_payload) {
        rkvc_g_free(g->model_payload);
        g->model_payload = NULL;
    }
}

/**
 * 为声明 bind_model 的节点选择并交付模型；载荷缓冲由图持有，节点销毁
 * 后释放。返回 NEGOTIATE 表示该候选应被淘汰（无兼容模型或节点拒绝
 * 契约）；INTEGRITY/IO 等原样上抛，不触发静默回退。
 */
static int bind_step_model(rkvc_graph *g, rkvc_node *n,
                           const rkvc_request *req, rkvc_diag **diag) {
    const rkvc_rkmodel *m;
    rkvc_model_binding binding;
    uint32_t kind = RKMODEL_PAYLOAD_RKNN;
    rkvc_status st;

    if (!g->ctx) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_INTERNAL, 1, n->ops->id,
                           "model binding requires a context");
        return (int)RKVC_STATUS_INTERNAL;
    }
    m = rkvc_model_registry_select(g->ctx, req, diag);
    if (!m) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NOT_FOUND, 1, n->ops->id,
                           "transform node requires a model but the "
                           "registry has no compatible candidate");
        return (int)RKVC_STATUS_NEGOTIATE;
    }
    /* 缺省交付 RKNN 载荷；容器未携带时回退到首个载荷（PMF 等角色）。 */
    if (!(m->info.payload_mask & (1u << RKMODEL_PAYLOAD_RKNN)) &&
        m->payload_count)
        kind = m->payloads[0].kind;

    release_model_payload(g);
    st = rkvc_rkmodel_load_payload(m, kind, &g->model_payload,
                                   &binding.payload_size);
    if (st != RKVC_STATUS_OK) {
        if (diag)
            rkvc_diag_push(diag, st, 1, n->ops->id,
                           "model payload load/verify failed");
        return (int)st;
    }
    binding.info = &m->info;
    binding.payload = g->model_payload;
    if (n->ops->bind_model(n, &binding, diag) != 0) {
        release_model_payload(g);
        return (int)RKVC_STATUS_NEGOTIATE;
    }
    return 0;
}

/** 逆序 close/destroy 前 created 个节点并释放连接队列。 */
static void reverse_release(rkvc_graph *g, size_t created) {
    size_t i;
    /* 逆序关闭已打开节点 */
    for (i = created; i > 0; --i) {
        rkvc_node *n = g->nodes[i - 1];
        if (n && n->state == RKVC_NODE_OPEN && n->ops->close)
            n->ops->close(n);
        if (n)
            n->state = RKVC_NODE_CLOSED;
    }
    /* 逆序销毁已创建节点 */
    for (i = created; i > 0; --i) {
        rkvc_node *n = g->nodes[i - 1];
        if (n && n->ops->destroy)
            n->ops->destroy(n);
        g->nodes[i - 1] = NULL;
    }
    /* 释放连接队列 */
    for (i = 0; i < g->queue_count; ++i)
        rkvc_queue_destroy(g->queues[i]);
    g->queue_count = 0;
}

int rkvc_graph_build(rkvc_graph *g, const rkvc_plan *plan, rkvc_diag **diag) {
    size_t i, created = 0;

    if (!g || !plan)
        return -2;
    g->failure_step = SIZE_MAX;
    g->node_count = plan->step_count;
    g->nodes = rkvc_g_calloc(g->node_count ? g->node_count : 1, sizeof(*g->nodes));
    g->queues = rkvc_g_calloc(g->node_count ? g->node_count : 1, sizeof(*g->queues));
    if (!g->nodes || !g->queues) {
        rkvc_g_free(g->nodes); g->nodes = NULL;
        rkvc_g_free(g->queues); g->queues = NULL;
        if (diag) rkvc_diag_push(diag, RKVC_STATUS_NOMEM, 1, "graph", "alloc");
        return (int)RKVC_STATUS_NOMEM;
    }

    for (i = 0; i < g->node_count; ++i) {
        rkvc_node *n = plan->steps[i].factory
                           ->create(plan->steps[i].factory, &plan->steps[i].request,
                                    plan->steps[i].factory->create_ctx);
        if (!n) {
            g->failure_step = i;
            if (diag) rkvc_diag_push(diag, RKVC_STATUS_INTERNAL, 1,
                    plan->steps[i].factory->id, "create failed");
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_INTERNAL;
        }
        n->graph = g;
        n->idx = i;
        n->state = RKVC_NODE_CREATED;
        g->nodes[i] = n;
        /* 声明 bind_model 的节点在 configure 前拿到已选模型（含 I/O 契约
         * 所需的载荷字节）；失败淘汰该候选并交由计划回退。 */
        if (n->ops->bind_model) {
            int brc = bind_step_model(g, n, &plan->steps[i].request, diag);
            if (brc != 0) {
                g->failure_step = i;
                reverse_release(g, i + 1); /* 节点 i 已创建，须一并销毁 */
                g->node_count = 0;
                return brc;
            }
        }
        created = i + 1;
    }

    /* 连接线性链：节点 i 的最后一个输出 → 节点 i+1 的第一个输入 */
    for (i = 0; i + 1 < g->node_count; ++i) {
        rkvc_node *src = g->nodes[i];
        rkvc_node *dst = g->nodes[i + 1];
        rkvc_port *out = src->out_count ? &src->out_ports[src->out_count - 1] : NULL;
        rkvc_port *in  = dst->in_count ? &dst->in_ports[0] : NULL;
        rkvc_queue *q;
        if (!out || !in) {
            g->failure_step = i + 1;
            if (diag) rkvc_diag_push(diag, RKVC_STATUS_NEGOTIATE, 1,
                    "graph", "linear link missing port");
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_NEGOTIATE;
        }
        q = rkvc_queue_create(g->queue_capacity ? g->queue_capacity : 4);
        if (!q) {
            if (diag) rkvc_diag_push(diag, RKVC_STATUS_NOMEM, 1, "graph", "queue alloc");
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_NOMEM;
        }
        out->queue = q;
        in->queue = q;
        g->queues[g->queue_count++] = q;
    }

    /* 协商（configure）：只定格式，不打开设备 */
    for (i = 0; i < g->node_count; ++i) {
        rkvc_node *n = g->nodes[i];
        if (n->ops->configure && n->ops->configure(n, diag) != 0) {
            g->failure_step = i;
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_NEGOTIATE;
        }
        n->state = RKVC_NODE_CONFIGURED;
    }

    /* configure 完成后统一解析每条边；不兼容格式必须在 open 设备前失败。 */
    for (i = 0; i + 1 < g->node_count; ++i) {
        rkvc_node *src = g->nodes[i];
        rkvc_node *dst = g->nodes[i + 1];
        rkvc_port *out = &src->out_ports[src->out_count - 1];
        rkvc_port *in = &dst->in_ports[0];
        if (negotiate_edge(out, in) != 0) {
            g->failure_step = i + 1;
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_NEGOTIATE, 1,
                               out->name ? out->name : "graph",
                               "incompatible connected port specs");
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_NEGOTIATE;
        }
    }

    g->state = 1;
    return 0;
}

int rkvc_graph_open(rkvc_graph *g, rkvc_diag **diag) {
    size_t i;

    if (!g)
        return (int)RKVC_STATUS_INVALID;
    if (g->state == 2)
        return 0;
    if (g->state != 1)
        return (int)RKVC_STATUS_INVALID;
    g->failure_step = SIZE_MAX;

    /* 实例化（open）：只在 job_start 阶段打开设备、分配大块内存。 */
    for (i = 0; i < g->node_count; ++i) {
        rkvc_node *n = g->nodes[i];
        if (n->ops->open && n->ops->open(n, diag) != 0) {
            g->failure_step = i;
            reverse_release(g, g->node_count);
            g->node_count = 0;
            g->state = 3;
            return (int)RKVC_STATUS_HW;
        }
        n->state = RKVC_NODE_OPEN;
    }

    g->state = 2;
    return 0;
}

int rkvc_graph_run(rkvc_graph *g, rkvc_diag **diag) {
    rkvc_exec *e;
    int rc;
    if (!g)
        return -2;
    if (g->state == 1) {
        rc = rkvc_graph_open(g, diag);
        if (rc != 0)
            return rc;
    }
    if (g->state != 2)
        return (int)RKVC_STATUS_INVALID;
    e = rkvc_exec_create(g, g->node_count);
    if (!e)
        return (int)RKVC_STATUS_NOMEM;
    g->exec = e;
    rc = rkvc_exec_run(e, diag);
    return rc;
}

void rkvc_graph_cancel(rkvc_graph *g) {
    if (g && g->exec)
        rkvc_exec_cancel((rkvc_exec *)g->exec);
}

void rkvc_graph_teardown(rkvc_graph *g) {
    if (!g || g->state == 3)
        return;
    if (g->exec) {
        rkvc_exec_destroy((rkvc_exec *)g->exec);
        g->exec = NULL;
    }
    reverse_release(g, g->node_count);
    g->node_count = 0;
    g->state = 3;
}

void rkvc_graph_free(rkvc_graph *g) {
    if (!g)
        return;
    rkvc_graph_teardown(g);
    /* 载荷在节点销毁之后释放：bind_model 节点可能持有其中的指针。 */
    release_model_payload(g);
    rkvc_g_free(g->nodes);
    rkvc_g_free(g->queues);
    rkvc_plan_release(&g->plan);
    rkvc_g_free(g);
}
