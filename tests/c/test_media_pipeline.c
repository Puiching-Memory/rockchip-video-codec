/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_media_pipeline.c
 * @brief 文件管线端到端回归（无硬件，x86 可跑）。
 *
 * 真实内建 fileio source/sink + fake 软件编解码后端，覆盖：
 * 规划器 SOURCE/SINK 步骤注入、执行器背压/EOS、帧搬运与文件 I/O 契约。
 * fake 编解码均为恒等搬运，因此文件进出字节必须完全一致。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <cmocka.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "graph_internal.h"
#include "context_internal.h"

/* ── fake 软件编解码后端（恒等搬运） ─────────────────────────────── */

typedef struct fake_node {
    int is_encode;
} fake_node;

/* rkvc_g_free 在 standalone 构建下是函数式宏，需经包装当函数指针使用。 */
static void fake_frame_release(void *ptr) {
    rkvc_g_free(ptr);
}

static void fake_destroy(rkvc_node *n) {
    rkvc_g_free(n->priv);
    rkvc_g_free(n->in_ports);
    rkvc_g_free(n->out_ports);
    rkvc_g_free(n);
}

static int fake_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec any = {0};
    (void)diag;
    if (node->in_count)
        rkvc_port_set_desired(&node->in_ports[0], &any);
    if (node->out_count)
        rkvc_port_set_desired(&node->out_ports[0], &any);
    return 0;
}

/* 恒等搬运：输入帧载荷原样作为输出帧（decode: es→raw；encode: raw→es）。 */
static int fake_process(rkvc_node *node, rkvc_frame *input,
                        rkvc_diag **diag) {
    fake_node *fn = node->priv;
    rkvc_frame_desc in_desc;
    rkvc_frame_desc out_desc;
    rkvc_frame *out = NULL;
    rkvc_status st;
    void *copy;
    int rc;
    (void)diag;

    if (rkvc_frame_get_desc(input, &in_desc) != RKVC_STATUS_OK || !in_desc.data)
        return (int)RKVC_STATUS_FORMAT;
    copy = rkvc_g_calloc(1, in_desc.size ? in_desc.size : 1);
    if (!copy)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(copy, in_desc.data, in_desc.size);

    rkvc_frame_desc_init(&out_desc, sizeof(out_desc));
    out_desc.spec = in_desc.spec;
    out_desc.spec.fmt = fn->is_encode ? RKVC_FRAME_FMT_BITSTREAM
                                      : RKVC_FRAME_FMT_NV12;
    out_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    if (!fn->is_encode) {
        /* 伪解码：捏造几何（宽=载荷、高=1），使 sink 逐行写回全部字节。 */
        out_desc.spec.width = (uint32_t)in_desc.size;
        out_desc.spec.stride = (uint32_t)in_desc.size;
        out_desc.spec.height = 1;
        out_desc.spec.ver_stride = 1;
    }
    out_desc.data = copy;
    out_desc.size = in_desc.size;
    st = rkvc_backend_frame_create(&out_desc, fake_frame_release, copy, &out);
    if (st != RKVC_STATUS_OK) {
        rkvc_g_free(copy);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, out);
    if (rc != 0)
        rkvc_frame_release(out);
    return rc;
}

static int fake_flush_noop(rkvc_node *node, rkvc_diag **diag) {
    (void)node;
    (void)diag;
    return 0;
}

static const rkvc_node_ops fake_dec_ops = {
    "fake.decode", fake_configure, NULL, fake_process, fake_flush_noop,
    NULL, fake_destroy,
};

static const rkvc_node_ops fake_enc_ops = {
    "fake.encode", fake_configure, NULL, fake_process, fake_flush_noop,
    NULL, fake_destroy,
};

static int fake_dec_matches(rkvc_operation op, rkvc_codec codec,
                            const rkvc_device_caps *caps) {
    (void)codec;
    (void)caps;
    return op == RKVC_OPERATION_DECODE || op == RKVC_OPERATION_TRANSCODE;
}

static int fake_enc_matches(rkvc_operation op, rkvc_codec codec,
                            const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_ENCODE || op == RKVC_OPERATION_TRANSCODE) &&
           (codec == RKVC_CODEC_H264 || codec == RKVC_CODEC_HEVC);
}

static rkvc_node *fake_create(const rkvc_node_factory *factory,
                              const rkvc_request *request, void *create_ctx) {
    int is_encode = factory->stage == RKVC_NODE_STAGE_ENCODE;
    rkvc_node *n = rkvc_g_calloc(1, sizeof(*n));
    fake_node *fn = rkvc_g_calloc(1, sizeof(*fn));
    (void)request;
    (void)create_ctx;
    if (!n || !fn) {
        rkvc_g_free(n);
        rkvc_g_free(fn);
        return NULL;
    }
    fn->is_encode = is_encode;
    n->ops = is_encode ? &fake_enc_ops : &fake_dec_ops;
    n->priv = fn;
    n->in_ports = rkvc_g_calloc(1, sizeof(*n->in_ports));
    n->out_ports = rkvc_g_calloc(1, sizeof(*n->out_ports));
    if (!n->in_ports || !n->out_ports) {
        fake_destroy(n);
        return NULL;
    }
    n->in_count = 1;
    n->in_ports[0].name = "in";
    n->in_ports[0].is_input = 1;
    n->out_count = 1;
    n->out_ports[0].name = "out";
    return n;
}

static const rkvc_node_factory fake_factories[] = {
    {
        .id = "fake.decode", .backend_id = "fakesw",
        .stage = RKVC_NODE_STAGE_DECODE, .priority = 100,
        .matches = fake_dec_matches, .create = fake_create,
    },
    {
        .id = "fake.encode", .backend_id = "fakesw",
        .stage = RKVC_NODE_STAGE_ENCODE, .priority = 100,
        .matches = fake_enc_matches, .create = fake_create,
    },
};

static const rkvc_node_factory *fake_factory_list(void *probe_ctx,
                                                  size_t *count) {
    (void)probe_ctx;
    *count = sizeof(fake_factories) / sizeof(fake_factories[0]);
    return fake_factories;
}

static const rkvc_backend fake_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "fakesw",
    .capability_flags = 0,
    .probe = NULL,
    .factories = fake_factory_list,
};

/* ── 工具 ─────────────────────────────────────────────────────────── */

static void make_path(char *out, size_t cap, const char *tag) {
    snprintf(out, cap, "rkvc_test_pipeline_%d_%s.bin", (int)getpid(), tag);
}

static void write_pattern_file(const char *path, size_t size, unsigned seed) {
    FILE *fp = fopen(path, "wb");
    unsigned char buf[4096];
    size_t off = 0;
    assert_non_null(fp);
    while (off < size) {
        size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
        size_t i;
        for (i = 0; i < n; ++i)
            buf[i] = (unsigned char)((off + i) * 131u + seed);
        assert_int_equal(fwrite(buf, 1, n, fp), n);
        off += n;
    }
    fclose(fp);
}

static void assert_files_equal(const char *a, const char *b) {
    FILE *fa = fopen(a, "rb");
    FILE *fb = fopen(b, "rb");
    unsigned char ba[4096], bb[4096];
    size_t na, nb;
    assert_non_null(fa);
    assert_non_null(fb);
    do {
        na = fread(ba, 1, sizeof(ba), fa);
        nb = fread(bb, 1, sizeof(bb), fb);
        assert_int_equal(na, nb);
        assert_memory_equal(ba, bb, na);
    } while (na);
    fclose(fa);
    fclose(fb);
}

static rkvc_status run_file_job(rkvc_context *ctx, rkvc_operation op,
                                rkvc_codec codec, const char *in,
                                const char *out, uint32_t w, uint32_t h) {
    rkvc_request req;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_status st;

    rkvc_request_init(&req, sizeof(req));
    req.operation = op;
    req.codec = codec;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = in;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = out;
    req.width = w;
    req.height = h;

    st = rkvc_job_create(ctx, &req, &diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, &diag);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);
    if (st != RKVC_STATUS_OK && diag) {
        char buf[1024];
        rkvc_diag_fmt_text(diag, buf, sizeof(buf));
        fprintf(stderr, "[run_file_job] st=%d diag: %s\n", (int)st, buf);
    }
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    return st;
}

static rkvc_context *ctx_with_fake(void) {
    rkvc_context *ctx = NULL;
    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &fake_backend),
                     RKVC_STATUS_OK);
    return ctx;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

/* 规划器为文件端点注入 SOURCE/SINK 步骤（decode: source+decode+sink）。 */
static void test_planner_injects_source_sink(void **state) {
    rkvc_context *ctx = ctx_with_fake();
    rkvc_device_caps caps = {0};
    rkvc_plan plan = {0};
    rkvc_diag *diag = NULL;
    rkvc_request req;
    (void)state;

    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    req.codec = RKVC_CODEC_H264;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = "in.h264";
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = "out.nv12";

    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);
    assert_int_equal(plan.step_count, 3);
    assert_int_equal(plan.steps[0].factory->stage, RKVC_NODE_STAGE_SOURCE);
    assert_int_equal(plan.steps[1].factory->stage, RKVC_NODE_STAGE_DECODE);
    assert_int_equal(plan.steps[2].factory->stage, RKVC_NODE_STAGE_SINK);
    assert_string_equal(plan.steps[0].factory->id, "file.source");
    assert_string_equal(plan.steps[2].factory->id, "file.sink");

    rkvc_plan_release(&plan);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

/* 流式端点不注入 source/sink（保持 job push/pull 路径）。 */
static void test_planner_streaming_no_fileio(void **state) {
    rkvc_context *ctx = ctx_with_fake();
    rkvc_device_caps caps = {0};
    rkvc_plan plan = {0};
    rkvc_diag *diag = NULL;
    rkvc_request req;
    (void)state;

    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    req.codec = RKVC_CODEC_H264;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;

    assert_int_equal(rkvc_plan_build(ctx, &req, &caps, &plan, &diag), 0);
    assert_int_equal(plan.step_count, 1);
    assert_int_equal(plan.steps[0].factory->stage, RKVC_NODE_STAGE_DECODE);

    rkvc_plan_release(&plan);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

/* decode 文件→文件：多 chunk（跨 256KB 边界）字节级一致。 */
static void test_decode_file_roundtrip(void **state) {
    char in[64], out[64];
    rkvc_context *ctx = ctx_with_fake();
    (void)state;

    make_path(in, sizeof(in), "dec_in");
    make_path(out, sizeof(out), "dec_out");
    write_pattern_file(in, 3 * 256 * 1024 + 123, 7);

    assert_int_equal(run_file_job(ctx, RKVC_OPERATION_DECODE,
                                  RKVC_CODEC_H264, in, out, 0, 0),
                     RKVC_STATUS_OK);
    assert_files_equal(in, out);

    remove(in);
    remove(out);
    rkvc_context_destroy(ctx);
}

/* encode 文件→文件：NV12 原始帧输入，尺寸由请求提供。 */
static void test_encode_file_roundtrip(void **state) {
    char in[64], out[64];
    rkvc_context *ctx = ctx_with_fake();
    const uint32_t w = 64, h = 48;
    (void)state;

    make_path(in, sizeof(in), "enc_in");
    make_path(out, sizeof(out), "enc_out");
    write_pattern_file(in, 3 * (size_t)w * h * 3 / 2, 11);

    assert_int_equal(run_file_job(ctx, RKVC_OPERATION_ENCODE,
                                  RKVC_CODEC_H264, in, out, w, h),
                     RKVC_STATUS_OK);
    assert_files_equal(in, out);

    remove(in);
    remove(out);
    rkvc_context_destroy(ctx);
}

/* transcode 文件→文件：decode+encode 两级恒等，字节不变。 */
static void test_transcode_file_roundtrip(void **state) {
    char in[64], out[64];
    rkvc_context *ctx = ctx_with_fake();
    (void)state;

    make_path(in, sizeof(in), "tc_in");
    make_path(out, sizeof(out), "tc_out");
    write_pattern_file(in, 100000, 3);

    assert_int_equal(run_file_job(ctx, RKVC_OPERATION_TRANSCODE,
                                  RKVC_CODEC_HEVC, in, out, 0, 0),
                     RKVC_STATUS_OK);
    assert_files_equal(in, out);

    remove(in);
    remove(out);
    rkvc_context_destroy(ctx);
}

/* 缺少编解码候选时 job 创建失败并给出 NOT_FOUND（仅剩 fileio）。 */
static void test_missing_codec_candidate(void **state) {
    rkvc_context *ctx = NULL;
    rkvc_request req;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    (void)state;

    assert_int_equal(rkvc_context_create(NULL, &ctx), RKVC_STATUS_OK);
    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    req.codec = RKVC_CODEC_H264;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = "no_such_input.h264";
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = "no_such_output.nv12";

    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job),
                     RKVC_STATUS_NOT_FOUND);
    assert_null(job);
    assert_non_null(diag); /* "required stage has no candidate" 诊断链 */

    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

/* 输入文件不存在：source open 失败，job 启动报错且不崩溃。 */
static void test_source_open_failure(void **state) {
    rkvc_context *ctx = ctx_with_fake();
    (void)state;

    assert_int_not_equal(run_file_job(ctx, RKVC_OPERATION_DECODE,
                                      RKVC_CODEC_H264,
                                      "definitely_missing_input.h264",
                                      "rkvc_test_pipeline_never.nv12", 0, 0),
                         RKVC_STATUS_OK);
    remove("rkvc_test_pipeline_never.nv12");
    rkvc_context_destroy(ctx);
}

/* encode 缺 width/height：协商期即失败（job 创建阶段报错）。 */
static void test_encode_requires_dimensions(void **state) {
    char in[64], out[64];
    rkvc_context *ctx = ctx_with_fake();
    (void)state;

    make_path(in, sizeof(in), "nodim_in");
    make_path(out, sizeof(out), "nodim_out");
    write_pattern_file(in, 1024, 5);

    assert_int_not_equal(run_file_job(ctx, RKVC_OPERATION_ENCODE,
                                      RKVC_CODEC_H264, in, out, 0, 0),
                         RKVC_STATUS_OK);

    remove(in);
    remove(out);
    rkvc_context_destroy(ctx);
}

/* 文件端点缺 uri：job 层直接拒绝（INVALID）。 */
static void test_file_endpoint_requires_uri(void **state) {
    rkvc_context *ctx = ctx_with_fake();
    rkvc_request req;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    (void)state;

    rkvc_request_init(&req, sizeof(req));
    req.operation = RKVC_OPERATION_DECODE;
    req.codec = RKVC_CODEC_H264;
    req.input.kind = RKVC_ENDPOINT_FILE;   /* uri = NULL */
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;

    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job),
                     RKVC_STATUS_INVALID);
    assert_null(job);

    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_planner_injects_source_sink),
        cmocka_unit_test(test_planner_streaming_no_fileio),
        cmocka_unit_test(test_decode_file_roundtrip),
        cmocka_unit_test(test_encode_file_roundtrip),
        cmocka_unit_test(test_transcode_file_roundtrip),
        cmocka_unit_test(test_missing_codec_candidate),
        cmocka_unit_test(test_source_open_failure),
        cmocka_unit_test(test_encode_requires_dimensions),
        cmocka_unit_test(test_file_endpoint_requires_uri),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
