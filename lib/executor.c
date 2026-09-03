/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file executor.c
 * @brief 通用图执行器：有界队列 + 每节点工作者线程。
 *
 * 背压、EOS、flush、cancel 与错误传播仅在此实现一次。队列为有界 FIFO，
 * 生产者写满时阻塞（背压）；下游节点按序消费，保证确定性顺序。
 */

#include "graph_internal.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

/* ── 队列 ─────────────────────────────────────────────────────────── */
/** @brief 有界 FIFO 帧队列：环形缓冲 + 互斥锁/条件变量实现背压与唤醒。 */
struct rkvc_queue {
    rkvc_frame **slots;          /**< 环形缓冲槽位 */
    size_t       capacity, head, count; /**< 容量 / 首帧下标 / 当前帧数 */
    pthread_mutex_t m;           /**< 保护以下全部字段 */
    pthread_cond_t  not_full;    /**< 消费后唤醒被背压的生产者 */
    pthread_cond_t  not_empty;   /**< 生产后唤醒等待的消费者 */
    int eos;                     /**< 流结束标记 */
    rkvc_exec *exec;             /**< 所属执行器（用于取消检查） */
};

/* ── 执行器 ───────────────────────────────────────────────────────── */
/** @brief 图执行器：每节点一个工作者线程 + 输入/输出边界队列。 */
struct rkvc_exec {
    rkvc_graph *g;               /**< 被执行的图 */
    pthread_t  *threads;         /**< 工作者线程数组 */
    size_t      nthreads;        /**< 计划启动的线程数 */
    size_t      started_threads; /**< 实际启动成功的线程数 */
    atomic_int  canceled;        /**< 取消标记（广播唤醒阻塞队列） */
    atomic_int  failed;          /**< 任一节点失败标记 */
    atomic_int  error_code;      /**< 首个失败的返回码 */
    rkvc_queue *input_queue;     /**< 图输入边界队列（job push 入口） */
    rkvc_queue *output_queue;    /**< 图输出边界队列（job pull 出口） */
    rkvc_queue **qlist;   /* 全部队列，用于 cancel 广播唤醒 */
    size_t      qcount, qcap;    /**< qlist 元素数与容量 */
    int         joined;          /**< 线程是否已全部 join */
};

rkvc_queue *rkvc_queue_create(size_t capacity) {
    rkvc_queue *q = rkvc_g_calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    if (!capacity)
        capacity = 4;
    q->slots = rkvc_g_calloc(capacity, sizeof(*q->slots));
    if (!q->slots) {
        rkvc_g_free(q);
        return NULL;
    }
    q->capacity = capacity;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->not_full, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    return q;
}

void rkvc_queue_destroy(rkvc_queue *q) {
    size_t i;
    if (!q)
        return;
    for (i = 0; i < q->count; ++i) {
        rkvc_frame *f = q->slots[(q->head + i) % q->capacity];
        rkvc_frame_release(f);
    }
    pthread_mutex_destroy(&q->m);
    pthread_cond_destroy(&q->not_full);
    pthread_cond_destroy(&q->not_empty);
    rkvc_g_free(q->slots);
    rkvc_g_free(q);
}

int rkvc_queue_push(rkvc_queue *q, rkvc_frame *f, void *execp) {
    rkvc_exec *e = execp;
    pthread_mutex_lock(&q->m);
    while (q->count == q->capacity && !q->eos &&
           !(e && atomic_load(&e->canceled))) {
        pthread_cond_wait(&q->not_full, &q->m);
    }
    if (e && atomic_load(&e->canceled)) {
        pthread_mutex_unlock(&q->m);
        return (int)RKVC_STATUS_CANCELED;
    }
    if (q->eos) {
        pthread_mutex_unlock(&q->m);
        return (int)RKVC_STATUS_EOF;
    }
    q->slots[(q->head + q->count) % q->capacity] = f;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->m);
    return 0;
}

int rkvc_queue_try_push(rkvc_queue *q, rkvc_frame *f, void *execp) {
    rkvc_exec *e = execp;
    int rc = 0;
    if (!q || !f)
        return (int)RKVC_STATUS_INVALID;
    pthread_mutex_lock(&q->m);
    if (e && atomic_load(&e->canceled))
        rc = (int)RKVC_STATUS_CANCELED;
    else if (q->eos)
        rc = (int)RKVC_STATUS_EOF;
    else if (q->count == q->capacity)
        rc = (int)RKVC_STATUS_AGAIN;
    else {
        q->slots[(q->head + q->count) % q->capacity] = f;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    }
    pthread_mutex_unlock(&q->m);
    return rc;
}

int rkvc_queue_pop(rkvc_queue *q, rkvc_frame **out, void *execp) {
    rkvc_exec *e = execp;
    pthread_mutex_lock(&q->m);
    while (q->count == 0 && !q->eos &&
           !(e && atomic_load(&e->canceled))) {
        pthread_cond_wait(&q->not_empty, &q->m);
    }
    if (q->count == 0) {
        int canceled = e && atomic_load(&e->canceled);
        pthread_mutex_unlock(&q->m);
        if (canceled)
            return (int)RKVC_STATUS_CANCELED;
        return 0; /* EOS */
    }
    *out = q->slots[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->m);
    return 1;
}

int rkvc_queue_try_pop(rkvc_queue *q, rkvc_frame **out, void *execp) {
    rkvc_exec *e = execp;
    int rc = 0;
    if (!q || !out)
        return (int)RKVC_STATUS_INVALID;
    pthread_mutex_lock(&q->m);
    if (e && atomic_load(&e->canceled))
        rc = (int)RKVC_STATUS_CANCELED;
    else if (q->count == 0)
        rc = q->eos ? 0 : (int)RKVC_STATUS_AGAIN;
    else {
        *out = q->slots[q->head];
        q->head = (q->head + 1) % q->capacity;
        q->count--;
        pthread_cond_signal(&q->not_full);
    }
    pthread_mutex_unlock(&q->m);
    return rc;
}

void rkvc_queue_set_eos(rkvc_queue *q) {
    pthread_mutex_lock(&q->m);
    q->eos = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->m);
}

/* ── 执行器 ───────────────────────────────────────────────────────── */
/** 把队列登记进执行器广播表（容量已预分配，满则忽略）。 */
static void exec_addq(rkvc_exec *e, rkvc_queue *q) {
    if (e->qcount == e->qcap)
        return;
    e->qlist[e->qcount++] = q;
}

/** 唤醒全部队列上阻塞的生产者/消费者（取消或失败时调用）。 */
static void exec_broadcast(rkvc_exec *e) {
    size_t i;
    for (i = 0; i < e->qcount; ++i) {
        pthread_mutex_lock(&e->qlist[i]->m);
        pthread_cond_broadcast(&e->qlist[i]->not_empty);
        pthread_cond_broadcast(&e->qlist[i]->not_full);
        pthread_mutex_unlock(&e->qlist[i]->m);
    }
}

/** 工作者线程参数：执行器 + 节点下标（线程自持有，退出前自释放）。 */
struct worker_arg { rkvc_exec *e; size_t idx; };

/** 节点主循环：从输入队列取帧 → process → 释放；EOS 时 flush 并下传 EOS。 */
static void *exec_worker(void *argp) {
    struct worker_arg *w = argp;
    rkvc_exec *e = w->e;
    rkvc_node *node = e->g->nodes[w->idx];
    rkvc_queue *inq = (w->idx == 0) ? e->input_queue : e->g->queues[w->idx - 1];
    rkvc_queue *outq = (w->idx + 1 == e->g->node_count)
                           ? e->output_queue : e->g->queues[w->idx];

    for (;;) {
        rkvc_frame *f = NULL;
        rkvc_diag *d = NULL;
        int r = rkvc_queue_pop(inq, &f, e);
        if (r < 0)
            break; /* 取消 */
        if (r == 0) { /* EOS */
            if (node->ops->flush && node->ops->flush(node, &d) != 0) {
                atomic_store(&e->error_code, (int)RKVC_STATUS_INTERNAL);
                atomic_store(&e->failed, 1);
            }
            rkvc_diag_release(d);
            rkvc_queue_set_eos(outq);
            break;
        }
        if (node->ops->process) {
            int rc = node->ops->process(node, f, &d);
            if (rc != 0) {
                atomic_store(&e->error_code,
                             rc < 0 ? rc : (int)RKVC_STATUS_INTERNAL);
                rkvc_diag_release(d);
                rkvc_frame_release(f);
                atomic_store(&e->failed, 1);
                atomic_store(&e->canceled, 1);
                exec_broadcast(e);
                break;
            }
        }
        rkvc_diag_release(d);
        rkvc_frame_release(f);
    }
    rkvc_g_free(w); /* 每个线程自持有其 worker_arg */
    return NULL;
}

struct rkvc_exec *rkvc_exec_create(rkvc_graph *g, size_t worker_threads) {
    rkvc_exec *e = rkvc_g_calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->g = g;
    e->nthreads = worker_threads;
    e->qcap = g->queue_count + 2;
    e->qlist = rkvc_g_calloc(e->qcap ? e->qcap : 1, sizeof(*e->qlist));
    if (!e->qlist) {
        rkvc_g_free(e);
        return NULL;
    }
    e->input_queue = rkvc_queue_create(g->queue_capacity ? g->queue_capacity : 4);
    e->output_queue = rkvc_queue_create(g->queue_capacity ? g->queue_capacity : 4);
    if (!e->input_queue || !e->output_queue) {
        rkvc_queue_destroy(e->input_queue);
        rkvc_queue_destroy(e->output_queue);
        rkvc_g_free(e->qlist);
        rkvc_g_free(e);
        return NULL;
    }
    exec_addq(e, e->input_queue);
    exec_addq(e, e->output_queue);
    for (size_t i = 0; i < g->queue_count; ++i)
        exec_addq(e, g->queues[i]);

    /* 链接首/末节点端口到执行器边界队列（流式 push/pull 出入口） */
    if (g->node_count) {
        rkvc_node *first = g->nodes[0];
        rkvc_node *last  = g->nodes[g->node_count - 1];
        if (first->in_count)
            first->in_ports[0].queue = e->input_queue;
        if (last->out_count)
            last->out_ports[last->out_count - 1].queue = e->output_queue;
    }
    return e;
}

int rkvc_exec_run(rkvc_exec *e, rkvc_diag **diag) {
    size_t i;
    int launch_error = 0;
    if (!e || e->nthreads == 0)
        return 0;
    e->threads = rkvc_g_calloc(e->nthreads, sizeof(*e->threads));
    if (!e->threads)
        return (int)RKVC_STATUS_NOMEM;
    for (i = 0; i < e->nthreads; ++i) {
        struct worker_arg *wa = rkvc_g_calloc(1, sizeof(*wa));
        if (!wa) {
            launch_error = (int)RKVC_STATUS_NOMEM;
            atomic_store(&e->canceled, 1);
            exec_broadcast(e);
            break;
        }
        wa->e = e;
        wa->idx = i;
        if (pthread_create(&e->threads[i], NULL, exec_worker, wa) != 0) {
            rkvc_g_free(wa);
            launch_error = (int)RKVC_STATUS_INTERNAL;
            atomic_store(&e->canceled, 1);
            exec_broadcast(e);
            break;
        }
        e->started_threads++;
    }
    for (i = 0; i < e->started_threads; ++i)
        pthread_join(e->threads[i], NULL);
    e->joined = 1;

    if (launch_error)
        return launch_error;

    if (atomic_load(&e->failed)) {
        int code = atomic_load(&e->error_code);
        if (diag)
            rkvc_diag_push(diag, (rkvc_status)code, 3,
                           "executor", "node failed");
        return code;
    }
    if (atomic_load(&e->canceled))
        return (int)RKVC_STATUS_CANCELED;
    return 0;
}

void rkvc_exec_cancel(rkvc_exec *e) {
    if (!e)
        return;
    atomic_store(&e->canceled, 1);
    exec_broadcast(e);
}

void rkvc_exec_destroy(rkvc_exec *e) {
    size_t i;
    if (!e)
        return;
    atomic_store(&e->canceled, 1);
    exec_broadcast(e);
    if (!e->joined && e->threads) {
        for (i = 0; i < e->started_threads; ++i)
            pthread_join(e->threads[i], NULL);
    }
    rkvc_queue_destroy(e->input_queue);
    rkvc_queue_destroy(e->output_queue);
    rkvc_g_free(e->threads);
    rkvc_g_free(e->qlist);
    rkvc_g_free(e);
}

/* ── 流式推/拉（供 job 层使用） ───────────────────────────────────── */
int rkvc_exec_push(rkvc_exec *e, rkvc_frame *frame) {
    if (!e)
        return (int)RKVC_STATUS_INVALID;
    return rkvc_queue_try_push(e->input_queue, frame, e);
}

int rkvc_exec_pull(rkvc_exec *e, rkvc_frame **frame) {
    int r;
    if (!e || !frame)
        return (int)RKVC_STATUS_INVALID;
    r = rkvc_queue_pop(e->output_queue, frame, e);
    if (r == 1)
        return 0;
    if (r == 0)
        return (int)RKVC_STATUS_EOF;
    return (int)RKVC_STATUS_CANCELED;
}

int rkvc_exec_try_pull(rkvc_exec *e, rkvc_frame **frame) {
    int r;
    if (!e || !frame)
        return (int)RKVC_STATUS_INVALID;
    r = rkvc_queue_try_pop(e->output_queue, frame, e);
    if (r == 1)
        return 0;
    if (r == 0)
        return (int)RKVC_STATUS_EOF;
    if (r == (int)RKVC_STATUS_AGAIN)
        return (int)RKVC_STATUS_AGAIN;
    return (int)RKVC_STATUS_CANCELED;
}

int rkvc_exec_eos(rkvc_exec *e) {
    if (!e)
        return (int)RKVC_STATUS_INVALID;
    rkvc_queue_set_eos(e->input_queue);
    return 0;
}
