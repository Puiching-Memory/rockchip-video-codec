/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file job.c
 * @brief rkvc_job：一次已规划执行的生命周期（公共 ABI 层）。
 *
 * create 只做规划、节点创建与格式协商；start 才打开设备并拉起执行线程；
 * 任一阶段失败均逆序回滚。wait/cancel/destroy 负责收尾。
 * 流式端点用 push/pull/push_eos 搬运帧；文件端点在 start 时自动 EOS
 * 输入队列，由源节点在 flush 阶段产出全部帧。
 */

#include "context_internal.h"
#include "graph_internal.h"

#include <pthread.h>
#include <string.h>

/** @brief rkvc_job 内部结构：规划结果 + 执行线程 + 完成同步。 */
struct rkvc_job {
    const rkvc_context *ctx;    /* 持有引用，保证工厂/节点 DSO 生命周期 */
    rkvc_request        req;    /* 请求深拷贝（uri/model_id 自有副本） */
    rkvc_graph         *graph;  /* 已构建/打开的图 */
    rkvc_exec          *exec;   /* == graph->exec */
    pthread_t           thread;      /* 执行线程（run 阻塞在此线程内） */
    pthread_mutex_t     mutex;       /* 保护以下生命周期字段 */
    pthread_cond_t      finished_cond; /* 等待执行线程完成 */
    int                 thread_started;
    int                 joining;     /* 某线程正在 join */
    int                 joined;      /* 执行线程已回收 */
    int                 finished;    /* 执行线程已置完成标记 */
    int                 state;  /* 0=planned 1=running 2=finished */
    int                 run_status;
    rkvc_diag          *diag;
    char               *input_uri;
    char               *output_uri;
    char               *model_id;
};

/** 库内 strdup（经 rkvc_g_calloc，便于统一审计）。 */
static char *job_strdup(const char *s) {
    char *copy;
    size_t len;
    if (!s)
        return NULL;
    len = strlen(s);
    copy = rkvc_g_calloc(len + 1, 1);
    if (copy)
        memcpy(copy, s, len + 1);
    return copy;
}

/** 释放请求字符串的自有副本。 */
static void job_free_strings(struct rkvc_job *j) {
    rkvc_g_free(j->input_uri);
    rkvc_g_free(j->output_uri);
    rkvc_g_free(j->model_id);
}

/** 反复 build 直到成功或候选耗尽；成功时计划所有权移交给图。 */
static int graph_build_with_fallback(rkvc_plan *plan, rkvc_diag **diag,
                                     rkvc_graph **out) {
    int rc;
    if (!plan || !out)
        return (int)RKVC_STATUS_INVALID;
    *out = NULL;
    for (;;) {
        size_t failed_step;
        rkvc_graph *graph = rkvc_graph_new();
        if (!graph)
            return (int)RKVC_STATUS_NOMEM;
        rc = rkvc_graph_build(graph, plan, diag);
        if (rc == 0) {
            graph->plan = *plan;
            memset(plan, 0, sizeof(*plan));
            *out = graph;
            return 0;
        }
        failed_step = graph->failure_step;
        rkvc_graph_free(graph);
        if ((rc != (int)RKVC_STATUS_NEGOTIATE &&
             rc != (int)RKVC_STATUS_INTERNAL) ||
            !rkvc_plan_advance(plan, failed_step))
            return rc;
        if (diag)
            rkvc_diag_push(diag, (rkvc_status)rc, 2, "planner",
                           "candidate rejected; trying fallback");
    }
}

/** open 失败（HW）时回退到次优候选重建图，直到成功或候选耗尽。 */
static int job_open_with_fallback(struct rkvc_job *job, rkvc_diag **diag) {
    for (;;) {
        size_t failed_step;
        int rc = rkvc_graph_open(job->graph, diag);
        rkvc_plan plan;
        if (rc == 0)
            return 0;
        if (rc != (int)RKVC_STATUS_HW)
            return rc;

        failed_step = job->graph->failure_step;
        plan = job->graph->plan;
        memset(&job->graph->plan, 0, sizeof(job->graph->plan));
        rkvc_graph_free(job->graph);
        job->graph = NULL;
        if (!rkvc_plan_advance(&plan, failed_step)) {
            rkvc_plan_release(&plan);
            return rc;
        }
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 2, "planner",
                           "open failed; trying fallback candidate");
        rc = graph_build_with_fallback(&plan, diag, &job->graph);
        if (rc != 0) {
            rkvc_plan_release(&plan);
            return rc;
        }
    }
}

/** 作业执行线程：跑执行器并在完成时广播 finished_cond。 */
static void *job_thread(void *argp) {
    struct rkvc_job *j = argp;
    j->run_status = rkvc_exec_run(j->exec, &j->diag);
    pthread_mutex_lock(&j->mutex);
    j->finished = 1;
    pthread_cond_broadcast(&j->finished_cond);
    pthread_mutex_unlock(&j->mutex);
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
    *out = NULL;
    if (req->header.struct_size <
            offsetof(rkvc_request, output) + sizeof(req->output) ||
        req->header.struct_size > (1u << 20) ||
        (req->header.api_version &&
         (req->header.api_version >> 16) != RKVC_ABI_VERSION_MAJOR))
        return RKVC_STATUS_INVALID;
    /* 文件端点必须携带 uri（规划器据此注入内建 source/sink 节点）。 */
    if ((req->input.kind == RKVC_ENDPOINT_FILE && !req->input.uri) ||
        (req->output.kind == RKVC_ENDPOINT_FILE && !req->output.uri))
        return RKVC_STATUS_INVALID;

    j = rkvc_g_calloc(1, sizeof(*j));
    if (!j)
        return RKVC_STATUS_NOMEM;
    j->ctx = ctx;
    memcpy(&j->req, req, req->header.struct_size < sizeof(j->req)
                         ? req->header.struct_size : sizeof(j->req));
    if (j->req.input.uri) {
        j->input_uri = job_strdup(j->req.input.uri);
        if (!j->input_uri) goto nomem;
        j->req.input.uri = j->input_uri;
    }
    if (j->req.output.uri) {
        j->output_uri = job_strdup(j->req.output.uri);
        if (!j->output_uri) goto nomem;
        j->req.output.uri = j->output_uri;
    }
    if (req->header.struct_size >=
            offsetof(rkvc_request, model_id) + sizeof(req->model_id) &&
        j->req.model_id) {
        j->model_id = job_strdup(j->req.model_id);
        if (!j->model_id) goto nomem;
        j->req.model_id = j->model_id;
        if (!rkvc_model_registry_select(ctx, &j->req, diag)) {
            job_free_strings(j);
            rkvc_g_free(j);
            return RKVC_STATUS_NOT_FOUND;
        }
    }
    pthread_mutex_init(&j->mutex, NULL);
    pthread_cond_init(&j->finished_cond, NULL);
    j->state = 0;

    rc = rkvc_plan_build(ctx, &j->req, &ctx->caps, &plan, diag);
    if (rc != 0) {
        pthread_cond_destroy(&j->finished_cond);
        pthread_mutex_destroy(&j->mutex);
        job_free_strings(j);
        rkvc_g_free(j);
        return (rkvc_status)rc;
    }

    rc = graph_build_with_fallback(&plan, diag, &j->graph);
    if (rc != 0) {
        rkvc_plan_release(&plan);
        pthread_cond_destroy(&j->finished_cond);
        pthread_mutex_destroy(&j->mutex);
        job_free_strings(j);
        rkvc_g_free(j);
        return (rkvc_status)rc;
    }
    /* 规划已由 graph_build_with_fallback 移交图持有。 */
    rkvc_context_retain(ctx);

    *out = j;
    return RKVC_STATUS_OK;
nomem:
    job_free_strings(j);
    rkvc_g_free(j);
    return RKVC_STATUS_NOMEM;
}

rkvc_status rkvc_job_start(rkvc_job *job, rkvc_diag **diag) {
    if (!job || !job->graph)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    if (job->state != 0) {
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_INVALID;
    }

    {
        int rc = job_open_with_fallback(job, diag);
        if (rc != 0) {
            job->state = 2;
            job->run_status = rc;
            job->joined = 1; /* 无执行线程；wait 直接返回启动失败状态 */
            pthread_mutex_unlock(&job->mutex);
            return (rkvc_status)rc;
        }
    }

    job->exec = rkvc_exec_create(job->graph, job->graph->node_count);
    if (!job->exec) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NOMEM, 3, "job", "exec alloc");
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_NOMEM;
    }
    job->graph->exec = job->exec;

    if (pthread_create(&job->thread, NULL, job_thread, job) != 0) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_INTERNAL, 3, "job",
                           "thread create failed");
        rkvc_exec_destroy(job->exec);
        job->exec = NULL;
        job->graph->exec = NULL;
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_INTERNAL;
    }
    job->thread_started = 1;
    job->state = 1;

    /* 文件端点：源节点不消费外部输入，直接 EOS 触发 flush 产出 */
    if (job->req.input.kind == RKVC_ENDPOINT_FILE)
        rkvc_exec_eos(job->exec);

    pthread_mutex_unlock(&job->mutex);
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_job_wait(rkvc_job *job) {
    pthread_t thread;
    rkvc_status result;
    if (!job)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    if (job->state == 0) {
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_INVALID;
    }
    while (job->joining && !job->joined)
        pthread_cond_wait(&job->finished_cond, &job->mutex);
    if (job->joined) {
        result = (rkvc_status)job->run_status;
        pthread_mutex_unlock(&job->mutex);
        return result;
    }
    job->joining = 1;
    thread = job->thread;
    pthread_mutex_unlock(&job->mutex);
    pthread_join(thread, NULL);
    pthread_mutex_lock(&job->mutex);
    job->thread_started = 0;
    job->joined = 1;
    job->joining = 0;
    job->state = 2;
    result = (rkvc_status)job->run_status;
    pthread_cond_broadcast(&job->finished_cond);
    pthread_mutex_unlock(&job->mutex);
    return result;
}

rkvc_status rkvc_job_cancel(rkvc_job *job) {
    if (!job)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    if (job->exec) rkvc_exec_cancel(job->exec);
    pthread_mutex_unlock(&job->mutex);
    return RKVC_STATUS_OK;
}

void rkvc_job_destroy(rkvc_job *job) {
    if (!job)
        return;
    rkvc_job_cancel(job);
    if (job->thread_started) (void)rkvc_job_wait(job);
    rkvc_diag_release(job->diag);
    rkvc_graph_free(job->graph); /* 内含 exec/teardown/plan 释放 */
    job_free_strings(job);
    rkvc_context_release((rkvc_context *)job->ctx);
    pthread_cond_destroy(&job->finished_cond);
    pthread_mutex_destroy(&job->mutex);
    rkvc_g_free(job);
}

rkvc_status rkvc_job_push(rkvc_job *job, rkvc_frame *frame) {
    rkvc_exec *exec;
    if (!job || !frame)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    if (job->finished) {
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_EOF;
    }
    exec = job->state == 1 ? job->exec : NULL;
    if (!exec) {
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_INVALID;
    }
    /* try_push 不阻塞；持锁可防止与生命周期状态切换交错。 */
    {
        rkvc_status st = (rkvc_status)rkvc_exec_push(exec, frame);
        pthread_mutex_unlock(&job->mutex);
        return st;
    }
}

rkvc_status rkvc_job_pull(rkvc_job *job, rkvc_frame **frame) {
    rkvc_exec *exec;
    if (!job || !frame)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    exec = job->state == 1 ? job->exec : NULL;
    pthread_mutex_unlock(&job->mutex);
    return exec ? (rkvc_status)rkvc_exec_pull(exec, frame) : RKVC_STATUS_INVALID;
}

rkvc_status rkvc_job_push_eos(rkvc_job *job) {
    rkvc_exec *exec;
    if (!job)
        return RKVC_STATUS_INVALID;
    pthread_mutex_lock(&job->mutex);
    exec = job->state == 1 ? job->exec : NULL;
    if (!exec) {
        pthread_mutex_unlock(&job->mutex);
        return RKVC_STATUS_INVALID;
    }
    {
        rkvc_status st = (rkvc_status)rkvc_exec_eos(exec);
        pthread_mutex_unlock(&job->mutex);
        return st;
    }
}
