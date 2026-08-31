/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file graph.c
 * @brief 0.4 通用图内核：节点/端口工具与图生命周期。
 *
 * 图构建分为协商（configure）与实例化（open）两步：协商只确定端口格式，
 * 实例化才打开设备与分配大块内存。任一步失败均按逆序释放已创建对象。
 */

#include "graph_internal.h"

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

int rkvc_node_emit(rkvc_node *n, int out_index, rkvc_frame *frame) {
    if (!n || !frame || out_index < 0 || out_index >= n->out_count)
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
        g->state = 0;
    }
    return g;
}

void rkvc_graph_set_queue_capacity(rkvc_graph *g, size_t capacity) {
    if (g && capacity)
        g->queue_capacity = capacity;
}

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
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_NEGOTIATE;
        }
        n->state = RKVC_NODE_CONFIGURED;
    }

    /* 实例化（open）：打开设备、分配大块内存 */
    for (i = 0; i < g->node_count; ++i) {
        rkvc_node *n = g->nodes[i];
        if (n->ops->open && n->ops->open(n, diag) != 0) {
            reverse_release(g, created);
            g->node_count = 0;
            return (int)RKVC_STATUS_HW;
        }
        n->state = RKVC_NODE_OPEN;
    }

    g->state = 1;
    return 0;
}

int rkvc_graph_run(rkvc_graph *g, rkvc_diag **diag) {
    rkvc_exec *e;
    int rc;
    if (!g)
        return -2;
    e = rkvc_exec_create(g, g->node_count);
    if (!e)
        return (int)RKVC_STATUS_NOMEM;
    g->exec = e;
    rc = rkvc_exec_run(e, diag);
    g->state = 2;
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
    rkvc_g_free(g->nodes);
    rkvc_g_free(g->queues);
    rkvc_plan_release(&g->plan);
    rkvc_g_free(g);
}
