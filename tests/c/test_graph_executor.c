/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_graph_executor.c
 * @brief 图内核单测（fake 节点，不依赖硬件）。
 *
 * 覆盖：规划逆序回滚、打开逆序回滚、背压（有界队列）、EOS/flush、取消、
 * 错误传播、确定性顺序、并发推帧。
 *
 * 独立编译：
 *   cc -DRKVC_STANDALONE_TEST -Iinclude -Ilib \
 *      tests/c/test_graph_executor.c lib/graph.c lib/executor.c lib/frame.c \
 *      lib/api.c lib/context.c -lcmocka -lpthread
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <cmocka.h>

#include "graph_internal.h"

/* ── 副作用追踪 ──────────────────────────────────────────────────── */
struct trace { int order[16]; int n; int flushes; };
static struct trace g_tr;

static void trace_push(int tag) {
    if (g_tr.n < 16)
        g_tr.order[g_tr.n++] = tag;
}
static int trace_count(void) { return g_tr.n; }

/* ── fake 节点 ────────────────────────────────────────────────────── */
struct fake_node {
    const char *tag;
    rkvc_frame_fmt fmt;
    int fail_configure, fail_open, fail_process, fail_flush;
    int processed, emitted, flush_count;
};

static int fake_configure(rkvc_node *n, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    rkvc_frame_spec spec;
    int i;
    if (fn->fail_configure) {
        if (diag) rkvc_diag_push(diag, RKVC_STATUS_NEGOTIATE, 1, fn->tag, "cfg");
        return -1;
    }
    spec.width = 640; spec.height = 480;
    spec.fmt = fn->fmt;
    spec.domain = RKVC_MEM_DOMAIN_HOST;
    spec.stride = 640; spec.modifier = 0;
    for (i = 0; i < n->in_count; ++i)
        rkvc_port_set_desired(&n->in_ports[i], &spec);
    for (i = 0; i < n->out_count; ++i)
        rkvc_port_set_desired(&n->out_ports[i], &spec);
    return 0;
}

static int fake_open(rkvc_node *n, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    if (fn->fail_open) {
        if (diag) rkvc_diag_push(diag, RKVC_STATUS_HW, 1, fn->tag, "open");
        return -1;
    }
    return 0;
}

static int fake_process(rkvc_node *n, rkvc_frame *in, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    if (fn->fail_process) {
        if (diag) rkvc_diag_push(diag, RKVC_STATUS_INTERNAL, 3, fn->tag, "proc");
        return -1;
    }
    fn->processed++;
    if (n->out_count && n->out_ports[0].queue) {
        fn->emitted++;
        return rkvc_node_emit(n, 0, rkvc_frame_retain(in));
    }
    return 0;
}

static int fake_flush(rkvc_node *n, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    (void)diag;
    fn->flush_count++;
    g_tr.flushes++;
    if (fn->fail_flush)
        return (int)RKVC_STATUS_HW;
    return 0;
}

static void fake_close(rkvc_node *n) {
    struct fake_node *fn = n->priv;
    trace_push(fn->tag[1] - '0'); /* tag 形如 "n0" */
}

static void fake_destroy(rkvc_node *n) {
    rkvc_g_free(n->priv);
    rkvc_g_free(n->in_ports);
    rkvc_g_free(n->out_ports);
    rkvc_g_free(n);
}

static const rkvc_node_ops fake_ops = {
    "fake", fake_configure, fake_open, fake_process,
    fake_flush, fake_close, fake_destroy,
};

/* ── 工厂（通过 create_ctx 注入 tag/失败标志） ────────────────────── */
struct fsetup { const char *tag; rkvc_frame_fmt fmt; int fc, fo, fp; };

static rkvc_node *setup_create(const rkvc_node_factory *f,
                               const rkvc_request *req, void *ctx) {
    struct fsetup *s = ctx;
    struct fake_node *fn;
    rkvc_node *n;
    (void)f; (void)req;
    fn = rkvc_g_calloc(1, sizeof(*fn));
    n = rkvc_g_calloc(1, sizeof(*n));
    if (!fn || !n) { rkvc_g_free(fn); rkvc_g_free(n); return NULL; }
    fn->tag = s ? s->tag : "n0";
    fn->fmt = s ? s->fmt : RKVC_FRAME_FMT_NV12;
    fn->fail_configure = s ? s->fc : 0;
    fn->fail_open = s ? s->fo : 0;
    fn->fail_process = s ? s->fp : 0;
    n->ops = &fake_ops;
    n->priv = fn;
    n->in_count = 1;
    n->in_ports = rkvc_g_calloc(1, sizeof(*n->in_ports));
    n->in_ports[0].name = "in"; n->in_ports[0].is_input = 1;
    n->out_count = 1;
    n->out_ports = rkvc_g_calloc(1, sizeof(*n->out_ports));
    n->out_ports[0].name = "out"; n->out_ports[0].is_input = 0;
    return n;
}

static int m_matches(rkvc_operation op, rkvc_codec c, const rkvc_device_caps *caps) {
    (void)op; (void)c; (void)caps; return 1;
}

/* ── 后端（含 K 个匹配工厂） ─────────────────────────────────────── */
#define MAXF 8
struct fake_backend {
    rkvc_backend     backend;
    rkvc_node_factory fs[MAXF];
    struct fsetup     setups[MAXF];
    size_t            count;
};

static const rkvc_node_factory *be_factories(void *ctx, size_t *count) {
    struct fake_backend *b = ctx;
    *count = b->count;
    return b->fs;
}
static int be_probe(const rkvc_device_caps *caps, void *ctx, rkvc_diag **diag) {
    (void)caps; (void)ctx;
    if (diag) *diag = NULL;
    return 0;
}

static void be_init(struct fake_backend *b, size_t count, const char *tags[],
                    const int fail[][3]) {
    static const rkvc_node_stage three_stages[] = {
        RKVC_NODE_STAGE_DECODE, RKVC_NODE_STAGE_TRANSFORM,
        RKVC_NODE_STAGE_ENCODE,
    };
    size_t i;
    memset(b, 0, sizeof(*b));
    b->backend.abi_version = RKVC_ABI_VERSION;
    b->backend.id = "fake";
    b->backend.probe = be_probe;
    b->backend.factories = be_factories;
    b->backend.probe_ctx = b;
    for (i = 0; i < count && i < MAXF; ++i) {
        b->setups[i].tag = tags[i];
        b->setups[i].fmt = RKVC_FRAME_FMT_NV12;
        b->setups[i].fc = fail[i][0];
        b->setups[i].fo = fail[i][1];
        b->setups[i].fp = fail[i][2];
        b->fs[i].id = tags[i];
        b->fs[i].backend_id = "fake";
        if (count == 1)
            b->fs[i].stage = RKVC_NODE_STAGE_DECODE;
        else if (count == 2)
            b->fs[i].stage = i ? RKVC_NODE_STAGE_ENCODE
                               : RKVC_NODE_STAGE_DECODE;
        else
            b->fs[i].stage = three_stages[i < 3 ? i : 2];
        b->fs[i].matches = m_matches;
        b->fs[i].create = setup_create;
        b->fs[i].create_ctx = &b->setups[i];
    }
    b->count = count;
}

/* ── 工具 ─────────────────────────────────────────────────────────── */
static rkvc_frame *mkframe(uint64_t seq) {
    rkvc_frame_spec spec;
    spec.width = 640; spec.height = 480;
    spec.fmt = RKVC_FRAME_FMT_NV12;
    spec.domain = RKVC_MEM_DOMAIN_HOST;
    spec.stride = 640; spec.modifier = 0;
    return rkvc_frame_internal_alloc(&spec, (void *)(uintptr_t)seq, -1, NULL, NULL);
}

static uint64_t seq_of(rkvc_frame *f) {
    void *d;
    rkvc_frame_get_data(f, &d, NULL);
    return (uint64_t)(uintptr_t)d;
}

struct run_arg { rkvc_exec *e; int rc; };
static void *exec_run_thunk(void *a) {
    struct run_arg *p = a;
    p->rc = rkvc_exec_run(p->e, NULL);
    return NULL;
}

/* ── 用例 1：规划/协商失败逆序回滚 ──────────────────────────────── */
static void test_configure_rollback(void **st) {
    const char *tags[] = {"n0", "n1", "n2"};
    const int fail[][3] = {{0,0,0}, {1,0,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 3, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    req.width = 640; /* 请求显式注入 transform 阶段 */
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);
    assert_int_equal(plan.step_count, 3);

    rkvc_graph *g = rkvc_graph_new();
    assert_non_null(g);
    g_tr.n = 0; g_tr.flushes = 0;
    assert_int_not_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(g->node_count, 0);       /* 全部已创建对象被逆序释放 */
    assert_int_equal(trace_count(), 0);       /* 协商失败 → 无已打开节点，无 close */
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* ── 用例 2：打开失败逆序回滚 ────────────────────────────────────── */
static void test_open_rollback(void **st) {
    const char *tags[] = {"n0", "n1", "n2"};
    const int fail[][3] = {{0,0,0}, {0,1,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 3, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    req.width = 640;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    assert_non_null(g);
    g_tr.n = 0;
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(trace_count(), 0);       /* create 只协商，不 open */
    assert_int_not_equal(rkvc_graph_open(g, &diag), 0);
    assert_int_equal(g->node_count, 0);
    assert_int_equal(trace_count(), 1);       /* 仅 n0 open 成功后逆序关闭 */
    assert_int_equal(g_tr.order[0], 0);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

static void test_incompatible_ports_rejected_before_open(void **st) {
    const char *tags[] = {"n0", "n1"};
    const int fail[][3] = {{0,0,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL;
    rkvc_diag *diag = NULL;
    rkvc_plan plan = {0};
    rkvc_device_caps caps = {0};
    rkvc_request req;
    rkvc_graph *g;
    (void)st;

    be_init(&be, 2, tags, fail);
    be.setups[1].fmt = RKVC_FRAME_FMT_RGB24;
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    g = rkvc_graph_new();
    assert_non_null(g);
    g_tr.n = 0;
    assert_int_equal(rkvc_graph_build(g, &plan, &diag),
                     RKVC_STATUS_NEGOTIATE);
    assert_int_equal(g->node_count, 0);
    assert_int_equal(trace_count(), 0);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan);
    rkvc_context_destroy(ctx);
    rkvc_diag_release(diag);
}

/* ── 用例 3：队列背压（有界） ────────────────────────────────────── */
struct bp_arg { rkvc_queue *q; volatile int pushed; };
static void *bp_push_thread(void *a) {
    struct bp_arg *arg = a;
    arg->pushed = rkvc_queue_push(arg->q, mkframe(1), NULL);
    return NULL;
}

static void test_queue_backpressure(void **st) {
    rkvc_queue *q; pthread_t th; struct bp_arg arg;
    rkvc_frame *f = NULL; int r;
    (void)st;

    q = rkvc_queue_create(1);
    assert_non_null(q);
    assert_int_equal(rkvc_queue_push(q, mkframe(1), NULL), 0); /* 占满 */
    arg.q = q; arg.pushed = 0;
    assert_int_equal(pthread_create(&th, NULL, bp_push_thread, &arg), 0);
    usleep(50 * 1000);
    assert_int_equal(arg.pushed, 0);           /* 容量 1 已满 → 阻塞 */
    r = rkvc_queue_pop(q, &f, NULL);
    assert_int_equal(r, 1);
    rkvc_frame_release(f);
    pthread_join(th, NULL);
    assert_int_equal(arg.pushed, 0);           /* 消费后 push 完成 */
    r = rkvc_queue_pop(q, &f, NULL);           /* 弹出线程插入的帧并释放 */
    assert_int_equal(r, 1);
    rkvc_frame_release(f);
    rkvc_queue_destroy(q);
}

/* ── 用例 4：公开输入面的 try-push 必须非阻塞 ───────────────────── */
static void test_queue_try_push_again(void **st) {
    rkvc_queue *q = rkvc_queue_create(1);
    rkvc_frame *first = mkframe(1);
    rkvc_frame *second = mkframe(2);
    rkvc_frame *out = NULL;
    (void)st;

    assert_non_null(q);
    assert_non_null(first);
    assert_non_null(second);
    assert_int_equal(rkvc_queue_try_push(q, first, NULL), RKVC_STATUS_OK);
    assert_int_equal(rkvc_queue_try_push(q, second, NULL), RKVC_STATUS_AGAIN);
    rkvc_frame_release(second); /* AGAIN：所有权未转移 */
    assert_int_equal(rkvc_queue_pop(q, &out, NULL), 1);
    assert_ptr_equal(out, first);
    rkvc_frame_release(out);

    rkvc_queue_set_eos(q);
    second = mkframe(3);
    assert_non_null(second);
    assert_int_equal(rkvc_queue_try_push(q, second, NULL), RKVC_STATUS_EOF);
    rkvc_frame_release(second);
    rkvc_queue_destroy(q);
}

/* try-pop must distinguish a delivered frame (1) from EOS (0).  Returning
 * zero after removing a frame makes the public try_pull path silently drop
 * every output and report EOF while the stream is still running. */
static void test_queue_try_pop_status(void **st) {
    rkvc_queue *q = rkvc_queue_create(1);
    rkvc_frame *in = mkframe(9);
    rkvc_frame *out = NULL;
    (void)st;

    assert_non_null(q);
    assert_non_null(in);
    assert_int_equal(rkvc_queue_try_pop(q, &out, NULL), RKVC_STATUS_AGAIN);
    assert_int_equal(rkvc_queue_try_push(q, in, NULL), RKVC_STATUS_OK);
    assert_int_equal(rkvc_queue_try_pop(q, &out, NULL), 1);
    assert_ptr_equal(out, in);
    rkvc_frame_release(out);
    rkvc_queue_set_eos(q);
    assert_int_equal(rkvc_queue_try_pop(q, &out, NULL), 0);
    rkvc_queue_destroy(q);
}

static int g_backend_frame_releases;

static void backend_frame_release(void *ctx) {
    int *count = ctx;
    (*count)++;
}

static void test_backend_frame_descriptor_and_release(void **st) {
    rkvc_frame_desc desc, got;
    rkvc_frame *frame = NULL;
    (void)st;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.width = 1920;
    desc.spec.height = 1080;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_DMABUF;
    desc.size = 3133440;
    desc.fd = 17;
    desc.pts = 9000;
    desc.dts = 8700;
    desc.flags = RKVC_FRAME_FLAG_KEYFRAME;
    g_backend_frame_releases = 0;

    assert_int_equal(rkvc_backend_frame_create(
                         &desc, backend_frame_release,
                         &g_backend_frame_releases, &frame), RKVC_STATUS_OK);
    assert_non_null(frame);
    assert_int_equal(rkvc_frame_get_desc(frame, &got), RKVC_STATUS_OK);
    assert_int_equal(got.fd, 17);
    assert_int_equal(got.size, desc.size);
    assert_int_equal(got.pts, 9000);
    assert_int_equal(got.dts, 8700);
    assert_int_equal(got.flags, RKVC_FRAME_FLAG_KEYFRAME);
    rkvc_frame_retain(frame);
    rkvc_frame_release(frame);
    assert_int_equal(g_backend_frame_releases, 0);
    rkvc_frame_release(frame);
    assert_int_equal(g_backend_frame_releases, 1);

    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    assert_int_equal(rkvc_frame_wrap(&desc, &frame), RKVC_STATUS_FORMAT);
    assert_null(frame);
}

static void test_backend_dmabuf_is_core_owned(void **st) {
    rkvc_frame_desc desc, got;
    rkvc_frame *frame = NULL;
    int pipefd[2];
    int owned_fd;
    (void)st;

    assert_int_equal(pipe(pipefd), 0);
    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.width = 64;
    desc.spec.height = 64;
    desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    desc.spec.domain = RKVC_MEM_DOMAIN_DMABUF;
    desc.fd = pipefd[0];
    assert_int_equal(rkvc_backend_frame_create_dmabuf(&desc, &frame),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_frame_get_desc(frame, &got), RKVC_STATUS_OK);
    owned_fd = got.fd;
    assert_true(owned_fd >= 0);
    assert_int_not_equal(owned_fd, pipefd[0]);
    close(pipefd[0]);
    close(pipefd[1]);
    assert_true(fcntl(owned_fd, F_GETFD) >= 0);
    rkvc_frame_release(frame);
    errno = 0;
    assert_int_equal(fcntl(owned_fd, F_GETFD), -1);
    assert_int_equal(errno, EBADF);
}

/* ── 用例 5：EOS/flush 传播（端到端顺序保留） ────────────────────── */
static void test_eos_flush_ordering(void **st) {
    const char *tags[] = {"n0", "n1", "n2"};
    const int fail[][3] = {{0,0,0}, {0,0,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 3, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    req.width = 640;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    rkvc_graph_set_queue_capacity(g, 16);
    assert_non_null(g);
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(rkvc_graph_open(g, &diag), 0);
    assert_int_equal(g->node_count, 3);

    rkvc_exec *e = rkvc_exec_create(g, 3);
    assert_non_null(e);
    g_tr.n = 0; g_tr.flushes = 0;

    pthread_t rth; struct run_arg ra = {e, 0};
    pthread_create(&rth, NULL, exec_run_thunk, &ra);
    usleep(20 * 1000);
    for (uint64_t i = 1; i <= 6; ++i)
        assert_int_equal(rkvc_exec_push(e, mkframe(i)), 0);
    assert_int_equal(rkvc_exec_eos(e), 0);
    pthread_join(rth, NULL);
    assert_int_equal(ra.rc, 0);
    assert_int_equal(g_tr.flushes, 3);         /* 每个节点各 flush 一次 */

    rkvc_frame *f; uint64_t expect = 1;
    while (rkvc_exec_pull(e, &f) == 0) {
        assert_int_equal(seq_of(f), expect++);
        rkvc_frame_release(f);
    }
    assert_int_equal(expect, 7);
    rkvc_exec_destroy(e);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* ── 用例 6：取消 ────────────────────────────────────────────────── */
static void test_cancel(void **st) {
    const char *tags[] = {"n0", "n1"};
    const int fail[][3] = {{0,0,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 2, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    assert_non_null(g);
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(rkvc_graph_open(g, &diag), 0);
    rkvc_exec *e = rkvc_exec_create(g, 2);
    assert_non_null(e);
    rkvc_exec_cancel(e);                       /* 无帧：唤醒阻塞的 worker */
    assert_int_equal(rkvc_exec_run(e, NULL), (int)RKVC_STATUS_CANCELED);
    rkvc_exec_destroy(e);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* ── 用例 7：错误传播（process 失败） ────────────────────────────── */
static void test_error_propagation(void **st) {
    const char *tags[] = {"n0", "n1", "n2"};
    const int fail[][3] = {{0,0,0}, {0,0,1}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 3, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_TRANSCODE;
    req.width = 640;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    assert_non_null(g);
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(rkvc_graph_open(g, &diag), 0);
    rkvc_exec *e = rkvc_exec_create(g, 3);
    assert_non_null(e);

    pthread_t rth; struct run_arg ra = {e, 0};
    pthread_create(&rth, NULL, exec_run_thunk, &ra);
    usleep(20 * 1000);
    rkvc_exec_push(e, mkframe(1));
    rkvc_exec_eos(e);
    pthread_join(rth, NULL);
    assert_true(ra.rc != 0);                   /* 错误传播，未挂起 */
    rkvc_exec_destroy(e);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* A flush failure is a terminal worker error, not a clean end-of-stream. */
static void test_flush_error_propagation(void **st) {
    const char *tags[] = {"n0"};
    const int fail[][3] = {{0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    rkvc_frame *frame = NULL;
    (void)st;

    be_init(&be, 1, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    assert_non_null(g);
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(rkvc_graph_open(g, &diag), 0);
    ((struct fake_node *)g->nodes[0]->priv)->fail_flush = 1;
    rkvc_exec *e = rkvc_exec_create(g, 1);
    assert_non_null(e);

    pthread_t rth; struct run_arg ra = {e, 0};
    pthread_create(&rth, NULL, exec_run_thunk, &ra);
    assert_int_equal(rkvc_exec_eos(e), 0);
    assert_int_equal(rkvc_exec_pull(e, &frame), (int)RKVC_STATUS_HW);
    assert_null(frame);
    pthread_join(rth, NULL);
    assert_int_equal(ra.rc, (int)RKVC_STATUS_HW);

    rkvc_exec_destroy(e);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* ── 用例 8：确定性规划顺序 ──────────────────────────────────────── */
static void test_plan_deterministic(void **st) {
    const char *tags[] = {"alpha", "beta", "gamma"};
    const int fail[][3] = {{0,0,0}, {0,0,0}, {0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 3, tags, fail);
    /* 同一阶段的多个候选：按 score 选出唯一一个；matches 不会级联。 */
    be.fs[0].stage = RKVC_NODE_STAGE_DECODE; be.fs[0].priority = 10;
    be.fs[1].stage = RKVC_NODE_STAGE_DECODE; be.fs[1].priority = 30;
    be.fs[2].stage = RKVC_NODE_STAGE_DECODE; be.fs[2].priority = 20;
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);
    assert_int_equal(plan.step_count, 1);
    assert_string_equal(plan.steps[0].factory->id, "beta");
    assert_int_equal(plan.steps[0].candidate_count, 3);
    assert_true(rkvc_plan_advance(&plan, 0));
    assert_string_equal(plan.steps[0].factory->id, "gamma");
    assert_true(rkvc_plan_advance(&plan, 0));
    assert_string_equal(plan.steps[0].factory->id, "alpha");
    assert_false(rkvc_plan_advance(&plan, 0));
    rkvc_plan_release(&plan);
    rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

/* ── 用例 9：并发推帧 + 顺序保留 ─────────────────────────────────── */
static void test_concurrent_ordering(void **st) {
    const char *tags[] = {"n0"};
    const int fail[][3] = {{0,0,0}};
    struct fake_backend be;
    rkvc_context *ctx = NULL; rkvc_diag *diag = NULL;
    rkvc_plan plan = {0}; rkvc_device_caps caps = {0};
    (void)st;

    be_init(&be, 1, tags, fail);
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &be.backend), RKVC_STATUS_OK);
    rkvc_request req; rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);

    rkvc_graph *g = rkvc_graph_new();
    rkvc_graph_set_queue_capacity(g, 16);
    assert_non_null(g);
    assert_int_equal(rkvc_graph_build(g, &plan, &diag), 0);
    assert_int_equal(rkvc_graph_open(g, &diag), 0);
    rkvc_exec *e = rkvc_exec_create(g, 1);
    assert_non_null(e);

    pthread_t rth; struct run_arg ra = {e, 0};
    pthread_create(&rth, NULL, exec_run_thunk, &ra);
    usleep(20 * 1000);
    for (uint64_t i = 1; i <= 5; ++i)
        rkvc_exec_push(e, mkframe(i));
    rkvc_exec_eos(e);
    pthread_join(rth, NULL);
    assert_int_equal(ra.rc, 0);

    rkvc_frame *f; uint64_t expect = 1;
    while (rkvc_exec_pull(e, &f) == 0) {
        assert_int_equal(seq_of(f), expect++);
        rkvc_frame_release(f);
    }
    assert_int_equal(expect, 6);
    rkvc_exec_destroy(e);
    rkvc_graph_free(g);
    rkvc_plan_release(&plan); rkvc_context_destroy(ctx); rkvc_diag_release(diag);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_configure_rollback),
        cmocka_unit_test(test_open_rollback),
        cmocka_unit_test(test_incompatible_ports_rejected_before_open),
        cmocka_unit_test(test_queue_backpressure),
        cmocka_unit_test(test_queue_try_push_again),
        cmocka_unit_test(test_queue_try_pop_status),
        cmocka_unit_test(test_backend_frame_descriptor_and_release),
        cmocka_unit_test(test_backend_dmabuf_is_core_owned),
        cmocka_unit_test(test_eos_flush_ordering),
        cmocka_unit_test(test_cancel),
        cmocka_unit_test(test_error_propagation),
        cmocka_unit_test(test_flush_error_propagation),
        cmocka_unit_test(test_plan_deterministic),
        cmocka_unit_test(test_concurrent_ordering),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
