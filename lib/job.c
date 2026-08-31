/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file job.c
 * @brief rkvc_job：一次已规划执行的生命周期（公共 ABI 层）。
 *
 * create 只做规划与协商/实例化（graph_build 内部先 configure 后 open，
 * 失败逆序回滚）；start 拉起执行线程；wait/cancel/destroy 收尾。
 * 流式端点用 push/pull/push_eos 搬运帧；文件端点在 start 时自动 EOS
 * 输入队列，由源节点在 flush 阶段产出全部帧。
 */

#include "context_internal.h"
#include "graph_internal.h"

#include <pthread.h>
#include <string.h>

struct rkvc_job {
    const rkvc_context *ctx;    /* 借用，不持有 */
    rkvc_request        req;
    rkvc_graph         *graph;
    rkvc_exec          *exec;   /* == graph->exec */
    pthread_t           thread;
    int                 thread_started;
    int                 state;  /* 0=planned 1=running 2=finished */
    int                 run_status;
    rkvc_diag          *diag;
};

static void *job_thread(void *argp) {
    struct rkvc_job *j = argp;
    j->run_status = rkvc_exec_run(j->exec, &j->diag);
    return NULL;
}

rkvc_status rkvc_job_create(const rkvc_context *ctx,
                            const rkvc_request *req,
                            rkvc_diag **diag,
                            rkvc_job **out) {
    struct rkvc_job *j;
    rkvc_plan plan = {0};
    int rc;

    if (!ctx || !req || !out)
        return RKVC_STATUS_INVALID;
    if (req->header.struct_size &&
        req->header.struct_size < offsetof(rkvc_request, model_id))
        return RKVC_STATUS_INVALID;

    j = rkvc_g_calloc(1, sizeof(*j));
    if (!j)
        return RKVC_STATUS_NOMEM;
    j->ctx = ctx;
    j->req = *req;
    j->state = 0;

    rc = rkvc_plan_build(ctx, &j->req, &ctx->caps, &plan, diag);
    if (rc != 0) {
        rkvc_g_free(j);
        return (rkvc_status)rc;
    }

    j->graph = rkvc_graph_new();
    if (!j->graph) {
        rkvc_plan_release(&plan);
        rkvc_g_free(j);
        return RKVC_STATUS_NOMEM;
    }

    rc = rkvc_graph_build(j->graph, &plan, diag);
    if (rc != 0) {
        rkvc_plan_release(&plan);
        rkvc_graph_free(j->graph);
        rkvc_g_free(j);
        return (rkvc_status)rc;
    }
    /* 规划移交图持有，随 graph_free 释放 */
    j->graph->plan = plan;

    *out = j;
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_job_start(rkvc_job *job, rkvc_diag **diag) {
    if (!job || !job->graph || job->state != 0)
        return RKVC_STATUS_INVALID;

    job->exec = rkvc_exec_create(job->graph, job->graph->node_count);
    if (!job->exec) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NOMEM, 3, "job", "exec alloc");
        return RKVC_STATUS_NOMEM;
    }
    job->graph->exec = job->exec;

    if (pthread_create(&job->thread, NULL, job_thread, job) != 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_INTERNAL, 3, "job",
                           "thread create failed");
        return RKVC_STATUS_INTERNAL;
    }
    job->thread_started = 1;
    job->state = 1;

    /* 文件端点：源节点不消费外部输入，直接 EOS 触发 flush 产出 */
    if (job->req.input.kind == RKVC_ENDPOINT_FILE)
        rkvc_exec_eos(job->exec);

    return RKVC_STATUS_OK;
}

rkvc_status rkvc_job_wait(rkvc_job *job) {
    if (!job)
        return RKVC_STATUS_INVALID;
    if (job->state == 0)
        return RKVC_STATUS_INVALID;
    if (job->thread_started) {
        pthread_join(job->thread, NULL);
        job->thread_started = 0;
    }
    job->state = 2;
    return (rkvc_status)job->run_status;
}

rkvc_status rkvc_job_cancel(rkvc_job *job) {
    if (!job)
        return RKVC_STATUS_INVALID;
    if (job->exec)
        rkvc_exec_cancel(job->exec);
    return RKVC_STATUS_OK;
}

void rkvc_job_destroy(rkvc_job *job) {
    if (!job)
        return;
    rkvc_job_cancel(job);
    if (job->thread_started) {
        pthread_join(job->thread, NULL);
        job->thread_started = 0;
    }
    rkvc_diag_release(job->diag);
    rkvc_graph_free(job->graph); /* 内含 exec/teardown/plan 释放 */
    rkvc_g_free(job);
}

rkvc_status rkvc_job_push(rkvc_job *job, rkvc_frame *frame) {
    if (!job || !frame || job->state != 1)
        return RKVC_STATUS_INVALID;
    return (rkvc_status)rkvc_exec_push(job->exec, frame);
}

rkvc_status rkvc_job_pull(rkvc_job *job, rkvc_frame **frame) {
    if (!job || !frame || job->state != 1)
        return RKVC_STATUS_INVALID;
    return (rkvc_status)rkvc_exec_pull(job->exec, frame);
}

rkvc_status rkvc_job_push_eos(rkvc_job *job) {
    if (!job || job->state != 1)
        return RKVC_STATUS_INVALID;
    return (rkvc_status)rkvc_exec_eos(job->exec);
}
