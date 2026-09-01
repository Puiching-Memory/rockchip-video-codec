/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_job.c
 * @brief rkvc_job 生命周期单测（fake 后端，不依赖硬件）。
 *
 * 覆盖：create 的规划失败、start/push/pull/push_eos/wait 流式往返、
 * cancel 语义、destroy 对运行中作业的回收。
 *
 * 独立编译（与 test_graph_executor 同一组源文件）。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <cmocka.h>

#include "graph_internal.h"
#include "context_internal.h"

/* ── fake 节点：透传帧 ───────────────────────────────────────────── */
struct fake_control { int fail_configure; int fail_open; };
struct fake_node { int processed; struct fake_control control; };
static int g_open_count;

static int fake_configure(rkvc_node *n, rkvc_diag **diag) {
    rkvc_frame_spec spec = {640, 480, RKVC_FRAME_FMT_NV12,
                            RKVC_MEM_DOMAIN_HOST, 640, 0};
    int i;
    struct fake_node *fn = n->priv;
    if (fn->control.fail_configure) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_NEGOTIATE, 1, "fake",
                           "configure failed");
        return -1;
    }
    for (i = 0; i < n->in_count; ++i)
        rkvc_port_set_desired(&n->in_ports[i], &spec);
    for (i = 0; i < n->out_count; ++i)
        rkvc_port_set_desired(&n->out_ports[i], &spec);
    return 0;
}

static int fake_process(rkvc_node *n, rkvc_frame *in, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    (void)diag;
    fn->processed++;
    if (n->out_count && n->out_ports[0].queue)
        return rkvc_node_emit(n, 0, rkvc_frame_retain(in));
    return 0;
}

static int fake_open(rkvc_node *n, rkvc_diag **diag) {
    struct fake_node *fn = n->priv;
    g_open_count++;
    if (fn->control.fail_open) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 1, "fake", "open failed");
        return -1;
    }
    return 0;
}

static void fake_destroy(rkvc_node *n) {
    rkvc_g_free(n->priv);
    rkvc_g_free(n->in_ports);
    rkvc_g_free(n->out_ports);
    rkvc_g_free(n);
}

static const rkvc_node_ops fake_ops = {
    "fake", fake_configure, fake_open, fake_process, NULL, NULL, fake_destroy,
};

static rkvc_node *fake_create(const rkvc_node_factory *f,
                              const rkvc_request *req, void *ctx) {
    rkvc_node *n;
    (void)f; (void)req;
    n = rkvc_g_calloc(1, sizeof(*n));
    if (!n)
        return NULL;
    n->ops = &fake_ops;
    n->priv = rkvc_g_calloc(1, sizeof(struct fake_node));
    if (n->priv && ctx)
        ((struct fake_node *)n->priv)->control =
            *(const struct fake_control *)ctx;
    n->in_count = 1;
    n->in_ports = rkvc_g_calloc(1, sizeof(*n->in_ports));
    n->in_ports[0].name = "in";
    n->in_ports[0].is_input = 1;
    n->out_count = 1;
    n->out_ports = rkvc_g_calloc(1, sizeof(*n->out_ports));
    n->out_ports[0].name = "out";
    if (!n->priv || !n->in_ports || !n->out_ports) {
        fake_destroy(n);
        return NULL;
    }
    return n;
}

static int fake_matches(rkvc_operation op, rkvc_codec c,
                        const rkvc_device_caps *caps) {
    (void)op; (void)c; (void)caps;
    return 1;
}

/* ── fake 后端 ────────────────────────────────────────────────────── */
static struct {
    rkvc_backend      backend;
    rkvc_node_factory factories[2];
    struct fake_control controls[2];
    size_t            factory_count;
} g_be;

static const rkvc_node_factory *g_factories(void *ctx, size_t *count) {
    (void)ctx;
    *count = g_be.factory_count;
    return g_be.factories;
}

static void setup_backend(void) {
    memset(&g_be, 0, sizeof(g_be));
    g_open_count = 0;
    g_be.backend.abi_version = RKVC_ABI_VERSION;
    g_be.backend.id = "fake";
    g_be.backend.factories = g_factories;
    g_be.factory_count = 1;
    g_be.factories[0].id = "fake.pass";
    g_be.factories[0].backend_id = "fake";
    g_be.factories[0].stage = RKVC_NODE_STAGE_TRANSFORM;
    g_be.factories[0].matches = fake_matches;
    g_be.factories[0].create = fake_create;
    g_be.factories[0].create_ctx = &g_be.controls[0];
}

static rkvc_frame *mkframe(uint8_t fill) {
    static uint8_t buf[640 * 480 * 3 / 2];
    rkvc_frame_spec spec = {640, 480, RKVC_FRAME_FMT_NV12,
                            RKVC_MEM_DOMAIN_HOST, 640, 0};
    rkvc_frame *f = NULL;
    memset(buf, fill, sizeof(buf));
    if (rkvc_frame_wrap_host(&spec, buf, sizeof(buf), &f) != RKVC_STATUS_OK)
        return NULL;
    return f;
}

static rkvc_request mkrequest(void) {
    rkvc_request req;
    memset(&req, 0, sizeof(req));
    req.header.struct_size = sizeof(req);
    req.header.api_version = RKVC_ABI_VERSION;
    req.operation = RKVC_OPERATION_UPSCALE;
    req.codec = RKVC_CODEC_H264;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    return req;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */
static void test_job_stream_roundtrip(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req = mkrequest();
    int i, pulled = 0;
    (void)state;

    setup_backend();
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_be.backend),
                     RKVC_STATUS_OK);

    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job), RKVC_STATUS_OK);
    assert_null(diag);
    assert_int_equal(g_open_count, 0); /* create 不得打开设备 */
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);
    assert_int_equal(g_open_count, 1);

    for (i = 0; i < 8; ++i) {
        rkvc_frame *f = mkframe((uint8_t)i);
        assert_non_null(f);
        assert_int_equal(rkvc_job_push(job, f), RKVC_STATUS_OK);
    }
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);

    for (;;) {
        rkvc_frame *f = NULL;
        rkvc_status st = rkvc_job_pull(job, &f);
        if (st == RKVC_STATUS_EOF)
            break;
        assert_int_equal(st, RKVC_STATUS_OK);
        assert_non_null(f);
        pulled++;
        rkvc_frame_release(f);
    }
    assert_int_equal(pulled, 8);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_job_destroy(job);
    rkvc_context_destroy(ctx);
}

static void test_job_retains_context(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_request req = mkrequest();
    rkvc_frame *in;
    rkvc_frame *out = NULL;
    (void)state;

    setup_backend();
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_be.backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, NULL, &job), RKVC_STATUS_OK);

    /* 应用释放自己的引用后，job 仍持有 context/后端生命周期。 */
    rkvc_context_destroy(ctx);
    assert_int_equal(rkvc_job_start(job, NULL), RKVC_STATUS_OK);
    in = mkframe(7);
    assert_non_null(in);
    assert_int_equal(rkvc_job_push(job, in), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(job, &out), RKVC_STATUS_OK);
    assert_non_null(out);
    rkvc_frame_release(out);
    assert_int_equal(rkvc_job_pull(job, &out), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);
    rkvc_job_destroy(job);
}

static void test_job_configure_falls_back_to_next_candidate(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req = mkrequest();
    (void)state;

    setup_backend();
    g_be.factory_count = 2;
    g_be.factories[0].id = "fake.bad-config";
    g_be.factories[0].priority = 100;
    g_be.controls[0].fail_configure = 1;
    g_be.factories[1] = g_be.factories[0];
    g_be.factories[1].id = "fake.fallback";
    g_be.factories[1].priority = 10;
    g_be.controls[1].fail_configure = 0;
    g_be.factories[1].create_ctx = &g_be.controls[1];

    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_be.backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job), RKVC_STATUS_OK);
    assert_non_null(diag);
    assert_int_equal(g_open_count, 0);
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);
    assert_int_equal(g_open_count, 1);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_diag_release(diag);
    rkvc_job_destroy(job);
    rkvc_context_destroy(ctx);
}

static void test_job_open_falls_back_to_next_candidate(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req = mkrequest();
    (void)state;

    setup_backend();
    g_be.factory_count = 2;
    g_be.factories[0].id = "fake.hardware";
    g_be.factories[0].priority = 100;
    g_be.controls[0].fail_open = 1;
    g_be.factories[1] = g_be.factories[0];
    g_be.factories[1].id = "fake.software";
    g_be.factories[1].priority = 10;
    g_be.controls[1].fail_open = 0;
    g_be.factories[1].create_ctx = &g_be.controls[1];

    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_be.backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);
    assert_int_equal(g_open_count, 2); /* 高优先级失败，次优候选成功 */
    assert_non_null(diag);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_diag_release(diag);
    rkvc_job_destroy(job);
    rkvc_context_destroy(ctx);
}

static void test_job_cancel(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_request req = mkrequest();
    (void)state;

    setup_backend();
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &g_be.backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, NULL, &job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_start(job, NULL), RKVC_STATUS_OK);

    assert_int_equal(rkvc_job_cancel(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_CANCELED);
    rkvc_job_destroy(job);
    rkvc_context_destroy(ctx);
}

static void test_job_create_no_backend(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req = mkrequest();
    (void)state;

    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job),
                     RKVC_STATUS_NOT_FOUND);
    assert_null(job);
    assert_non_null(diag); /* 规划失败必须给出可解释诊断 */
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

static void test_job_invalid_args(void **state) {
    rkvc_request req = mkrequest();
    rkvc_job *job = NULL;
    (void)state;

    assert_int_equal(rkvc_job_create(NULL, &req, NULL, &job),
                     RKVC_STATUS_INVALID);
    assert_int_equal(rkvc_job_create((rkvc_context *)0x1, NULL, NULL, &job),
                     RKVC_STATUS_INVALID);
    assert_int_equal(rkvc_job_wait(NULL), RKVC_STATUS_INVALID);
    assert_int_equal(rkvc_job_cancel(NULL), RKVC_STATUS_INVALID);
    rkvc_job_destroy(NULL); /* 无操作 */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_job_stream_roundtrip),
        cmocka_unit_test(test_job_retains_context),
        cmocka_unit_test(test_job_configure_falls_back_to_next_candidate),
        cmocka_unit_test(test_job_open_falls_back_to_next_candidate),
        cmocka_unit_test(test_job_cancel),
        cmocka_unit_test(test_job_create_no_backend),
        cmocka_unit_test(test_job_invalid_args),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
