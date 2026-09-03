/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_model_binding.c
 * @brief transform 节点模型绑定链路回归（无硬件，x86 可跑）。
 *
 * 覆盖核心 bind_model 交付契约：
 *  - 图构建期按请求选择模型，载荷字节交给优先候选；
 *  - 注册表无兼容模型时首选被淘汰并回退到无模型 transform；
 *  - 注册表扫描后载荷文件被截断时不回退，job 创建以 IO 失败。
 */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdio.h>
#include <cmocka.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "graph_internal.h"
#include "context_internal.h"
#include "rkmodel.h"

/* ── .rkmodel 容器构造（对齐 test_rkmodel.c 的字节序契约） ────────── */

typedef struct {
    uint8_t *bytes;
    size_t   len;
    size_t   cap;
} blob;

static void blob_put(blob *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 128;
        b->bytes = realloc(b->bytes, b->cap);
    }
    memcpy(b->bytes + b->len, p, n);
    b->len += n;
}

static void blob_u16(blob *b, uint16_t v) { blob_put(b, &v, 2); }
static void blob_u32(blob *b, uint32_t v) { blob_put(b, &v, 4); }

static void blob_tlv(blob *b, uint16_t tag, const char *s) {
    blob_u16(b, tag);
    blob_u32(b, (uint32_t)strlen(s));
    blob_put(b, s, strlen(s));
}

#define MODEL_PAYLOAD_LEN 128u

/** 单 RKNN 载荷容器；返回载荷数据区文件偏移。 */
static size_t build_container(blob *out, const uint8_t payload[MODEL_PAYLOAD_LEN]) {
    blob tlv = {0};
    rkmodel_fixed fx;
    rkmodel_payload_entry entry;
    size_t table_off;

    blob_tlv(&tlv, RKMODEL_TAG_FAMILY, "sr");
    blob_tlv(&tlv, RKMODEL_TAG_ROLE, "upscale");
    blob_tlv(&tlv, RKMODEL_TAG_ID, "sr-x3-test");
    blob_tlv(&tlv, RKMODEL_TAG_VERSION, "1.0.0");

    memset(&fx, 0, sizeof(fx));
    fx.magic = RKMODEL_MAGIC;
    fx.format_version = RKMODEL_VERSION;
    fx.header_len = (uint32_t)tlv.len;
    fx.payload_count = 1;
    fx.payload_entry_size = sizeof(rkmodel_payload_entry);

    table_off = RKMODEL_FIXED_SIZE + tlv.len + sizeof(entry);
    memset(&entry, 0, sizeof(entry));
    entry.kind = RKMODEL_PAYLOAD_RKNN;
    entry.offset = table_off;
    entry.length = MODEL_PAYLOAD_LEN;
    memset(out, 0, sizeof(*out));
    blob_put(out, &fx, sizeof(fx));
    blob_put(out, tlv.bytes, tlv.len);
    blob_put(out, &entry, sizeof(entry));
    blob_put(out, payload, MODEL_PAYLOAD_LEN);
    free(tlv.bytes);
    return table_off;
}

static void write_file(const char *path, const uint8_t *p, size_t n) {
    FILE *f = fopen(path, "wb");
    assert_non_null(f);
    assert_int_equal(fwrite(p, 1, n, f), n);
    fclose(f);
}

/* ── fake transform 后端：bind_model 记录型 + 无模型型 ────────────── */

static uint8_t g_model_payload[MODEL_PAYLOAD_LEN];
static size_t  g_bind_count;
static char    g_bound_id[64];

static void frame_free(void *ptr) {
    rkvc_g_free(ptr);
}

/** 恒等搬运 process：分配新缓冲承载输入载荷并发出（两工厂共用）。 */
static int echo_process(rkvc_node *node, rkvc_frame *input, rkvc_diag **diag) {
    rkvc_frame_desc desc;
    rkvc_frame_desc out_desc;
    rkvc_frame *out = NULL;
    void *copy;
    int rc;
    (void)diag;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        !desc.data || !desc.size)
        return (int)RKVC_STATUS_FORMAT;
    copy = rkvc_g_calloc(1, desc.size);
    if (!copy)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(copy, desc.data, desc.size);

    rkvc_frame_desc_init(&out_desc, sizeof(out_desc));
    out_desc.spec = desc.spec;
    out_desc.data = copy;
    out_desc.size = desc.size;
    if (rkvc_backend_frame_create(&out_desc, frame_free, copy, &out)
        != RKVC_STATUS_OK) {
        rkvc_g_free(copy);
        return (int)RKVC_STATUS_NOMEM;
    }
    rc = rkvc_node_emit(node, 0, out);
    if (rc != 0)
        rkvc_frame_release(out);
    return rc;
}

static int any_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec any = {0};
    (void)diag;
    if (node->in_count)
        rkvc_port_set_desired(&node->in_ports[0], &any);
    if (node->out_count)
        rkvc_port_set_desired(&node->out_ports[0], &any);
    return 0;
}

static void fake_destroy(rkvc_node *n) {
    rkvc_g_free(n->priv);
    rkvc_g_free(n->in_ports);
    rkvc_g_free(n->out_ports);
    rkvc_g_free(n);
}

/** bind_model：记录交付内容并校验载荷与容器摘要一致。 */
static int sr_bind_model(rkvc_node *node, const rkvc_model_binding *model,
                         rkvc_diag **diag) {
    (void)node;
    (void)diag;
    if (!model || !model->info || !model->payload ||
        model->payload_size != MODEL_PAYLOAD_LEN)
        return (int)RKVC_STATUS_FORMAT;
    if (strcmp(model->info->role, "upscale") != 0)
        return (int)RKVC_STATUS_FORMAT;
    memcpy(g_model_payload, model->payload, MODEL_PAYLOAD_LEN);
    snprintf(g_bound_id, sizeof(g_bound_id), "%s", model->info->id);
    g_bind_count++;
    return 0;
}

static const rkvc_node_ops sr_ops = {
    "fake.sr", any_configure, NULL, echo_process, NULL, NULL, fake_destroy,
    sr_bind_model,
};

static const rkvc_node_ops scale_ops = {
    "fake.scale", any_configure, NULL, echo_process, NULL, NULL, fake_destroy,
};

static rkvc_node *transform_create(const rkvc_node_factory *factory,
                                   const rkvc_request *request,
                                   void *create_ctx) {
    rkvc_node *n = rkvc_g_calloc(1, sizeof(*n));
    (void)request;
    (void)create_ctx;
    if (!n)
        return NULL;
    n->ops = strcmp(factory->id, "fake.sr") == 0 ? &sr_ops : &scale_ops;
    n->in_ports = rkvc_g_calloc(1, sizeof(*n->in_ports));
    n->out_ports = rkvc_g_calloc(1, sizeof(*n->out_ports));
    if (!n->in_ports || !n->out_ports) {
        fake_destroy(n);
        return NULL;
    }
    n->in_count = 1;
    n->in_ports[0].name = "video";
    n->in_ports[0].is_input = 1;
    n->out_count = 1;
    n->out_ports[0].name = "video";
    return n;
}

static int upscale_matches(rkvc_operation op, rkvc_codec c,
                           const rkvc_device_caps *caps) {
    (void)c;
    (void)caps;
    return op == RKVC_OPERATION_UPSCALE;
}

static const rkvc_node_factory sr_factories[] = {
    {"fake.sr", "fake-sr", RKVC_NODE_STAGE_TRANSFORM, 900, upscale_matches,
     NULL, transform_create, NULL},
};

static const rkvc_node_factory scale_factories[] = {
    {"fake.scale", "fake-scale", RKVC_NODE_STAGE_TRANSFORM, 500,
     upscale_matches, NULL, transform_create, NULL},
};

static const rkvc_node_factory *factory_list_sr(void *ctx, size_t *count) {
    (void)ctx;
    *count = 1;
    return sr_factories;
}

static const rkvc_node_factory *factory_list_scale(void *ctx, size_t *count) {
    (void)ctx;
    *count = 1;
    return scale_factories;
}

static const rkvc_backend sr_backend = {
    RKVC_ABI_VERSION, "fake-sr", RKVC_BACKEND_CAP_RKNN, NULL, factory_list_sr,
};

static const rkvc_backend scale_backend = {
    RKVC_ABI_VERSION, "fake-scale", 0, NULL, factory_list_scale,
};

/* ── 公共夹具 ─────────────────────────────────────────────────────── */

#define MODEL_DIR "/tmp/rkvc_test_binding/models"

/** 生成确定性的 RKNN 载荷内容。 */
static void fill_payload(uint8_t payload[MODEL_PAYLOAD_LEN]) {
    for (uint32_t i = 0; i < MODEL_PAYLOAD_LEN; ++i)
        payload[i] = (uint8_t)(i * 7 + 1);
}

static rkvc_context *make_context(const char *model_dir) {
    rkvc_context_options opts;
    rkvc_context *ctx = NULL;
    rkvc_context_options_init(&opts, sizeof(opts));
    opts.model_dir_override = model_dir;
    assert_int_equal(rkvc_context_create(&opts, &ctx), RKVC_STATUS_OK);
    return ctx;
}

static rkvc_request upscale_request(void) {
    rkvc_request req;
    memset(&req, 0, sizeof(req));
    req.header.struct_size = sizeof(req);
    req.header.api_version = RKVC_ABI_VERSION;
    req.operation = RKVC_OPERATION_UPSCALE;
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_NV12;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_NV12;
    return req;
}

static rkvc_frame *mkframe(void) {
    static uint8_t buf[64 * 64 * 3 / 2];
    rkvc_frame_spec spec = {64, 64, RKVC_FRAME_FMT_NV12,
                            RKVC_MEM_DOMAIN_HOST, 64, 64};
    rkvc_frame *f = NULL;
    memset(buf, 0xa5, sizeof(buf));
    assert_int_equal(rkvc_frame_wrap_host(&spec, buf, sizeof(buf), &f),
                     RKVC_STATUS_OK);
    return f;
}

/* ── 用例 ─────────────────────────────────────────────────────────── */

/** 有兼容模型：首选工厂在 create 期收到校验过的载荷与摘要。 */
static void test_bind_model_receives_payload(void **state) {
    uint8_t payload[MODEL_PAYLOAD_LEN];
    blob container = {0};
    rkvc_context *ctx;
    rkvc_request req = upscale_request();
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_frame *in;
    rkvc_frame *out = NULL;
    (void)state;

    fill_payload(payload);
    build_container(&container, payload);
    mkdir("/tmp/rkvc_test_binding", 0755);
    mkdir(MODEL_DIR, 0755);
    write_file(MODEL_DIR "/sr-x3.rkmodel", container.bytes, container.len);

    g_bind_count = 0;
    ctx = make_context(MODEL_DIR);
    assert_int_equal(rkvc_registry_add_backend(ctx, &sr_backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &scale_backend),
                     RKVC_STATUS_OK);
    /* 绑定发生在 job 创建（图构建）期，无需 start。 */
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job), RKVC_STATUS_OK);
    assert_int_equal(g_bind_count, 1);
    assert_string_equal(g_bound_id, "sr-x3-test");
    assert_memory_equal(g_model_payload, payload, MODEL_PAYLOAD_LEN);

    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);
    in = mkframe();
    /* push 成功后所有权已转移给作业，此处不得再 release。 */
    assert_int_equal(rkvc_job_push(job, in), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(job, &out), RKVC_STATUS_OK);
    assert_non_null(out);
    {
        rkvc_frame_spec spec;
        assert_int_equal(rkvc_frame_get_spec(out, &spec), RKVC_STATUS_OK);
        assert_int_equal(spec.fmt, RKVC_FRAME_FMT_NV12);
    }
    rkvc_frame_release(out);
    assert_int_equal(rkvc_job_pull(job, &out), RKVC_STATUS_EOF);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    free(container.bytes);
}

/** 注册表为空：首选因无模型被淘汰，回退到无模型 transform 并跑通。 */
static void test_fallback_when_no_model(void **state) {
    rkvc_context *ctx;
    rkvc_request req = upscale_request();
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_frame *in;
    rkvc_frame *out = NULL;
    (void)state;

    mkdir("/tmp/rkvc_test_binding_empty", 0755);
    g_bind_count = 0;
    ctx = make_context("/tmp/rkvc_test_binding_empty");
    assert_int_equal(rkvc_registry_add_backend(ctx, &sr_backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_registry_add_backend(ctx, &scale_backend),
                     RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job), RKVC_STATUS_OK);
    assert_int_equal(g_bind_count, 0); /* sr 淘汰，scale 无绑定 */

    assert_int_equal(rkvc_job_start(job, &diag), RKVC_STATUS_OK);
    in = mkframe();
    /* push 成功后所有权已转移给作业，此处不得再 release。 */
    assert_int_equal(rkvc_job_push(job, in), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_push_eos(job), RKVC_STATUS_OK);
    assert_int_equal(rkvc_job_pull(job, &out), RKVC_STATUS_OK);
    rkvc_frame_release(out);
    assert_int_equal(rkvc_job_wait(job), RKVC_STATUS_OK);

    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
}

/** 扫描后载荷文件被截断：不静默回退，job 创建以 IO 失败。 */
static void test_truncated_payload_rejected(void **state) {
    uint8_t payload[MODEL_PAYLOAD_LEN];
    blob container = {0};
    rkvc_context *ctx;
    rkvc_request req = upscale_request();
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    size_t data_off;
    (void)state;

    fill_payload(payload);
    data_off = build_container(&container, payload);
    container.bytes[data_off + 7] ^= 0xff; /* 破坏载荷区一个字节 */
    const char *path = "/tmp/rkvc_test_binding_truncated/models/bad.rkmodel";
    mkdir("/tmp/rkvc_test_binding_truncated", 0755);
    mkdir("/tmp/rkvc_test_binding_truncated/models", 0755);
    write_file(path,
               container.bytes, container.len);

    g_bind_count = 0;
    ctx = make_context("/tmp/rkvc_test_binding_truncated/models");
    assert_int_equal(rkvc_model_count(ctx), 1);
    assert_int_equal(truncate(path, (off_t)(data_off + 7)), 0);
    assert_int_equal(rkvc_registry_add_backend(ctx, &sr_backend),
                     RKVC_STATUS_OK); /* 仅有模型工厂：无处回退 */
    assert_int_equal(rkvc_job_create(ctx, &req, &diag, &job),
                     RKVC_STATUS_IO);
    assert_int_equal(g_bind_count, 0);
    assert_null(job);

    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    free(container.bytes);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_bind_model_receives_payload),
        cmocka_unit_test(test_fallback_when_no_model),
        cmocka_unit_test(test_truncated_payload_rejected),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
