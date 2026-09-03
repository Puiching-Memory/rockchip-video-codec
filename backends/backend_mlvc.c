/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_mlvc.c
 * @brief MLVC 神经视频编解码后端（RKNN NPU + rANS 熵编码）。
 *
 * 0.4 适配：模型（RKNN + 双 PMF 熵表 + 可选 QPPATCH）由核心经
 * bind_model() 多载荷视图交付。
 *
 * - mlvc.encode（ENCODE stage）：NV12 帧 → encoder NPU → rANS 码流，
 *   每帧输出一条完整 .mlvc 记录（首帧附 32B 容器头）的 BITSTREAM 帧；
 *   file.sink 直写即得合法 .mlvc 文件。
 * - mlvc.decode（DECODE stage）：BITSTREAM 块 → 流式 demux（.mlvc 容器）
 *   → rANS 解码 → decoder NPU → NV12 帧。qp 取自容器头；RKNN 惰性
 *   初始化（首帧头部解析后、qp 已知时应用 QPPATCH 再 rknn_init）。
 *
 * RKNN I/O 模式（移植自 0.3 node_mlvc.c，双板验证过）：
 *   encoder：native 零拷贝输入（NHWC 像素 + NC1HWC2 参考）+ 逻辑输出；
 *   native attr 不兼容时回退标准 host 输入。decoder：全 native 零拷贝。
 */

#include "mlvc/container.h"
#include "mlvc/mlvc_pixel.h"
#include "mlvc/pmf.h"
#include "mlvc/qppatch.h"
#include "mlvc/rans.h"
#include "rkvc/backend.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkmodel_layout.h"
#include <rknn_api.h>

/* ── RKNN 运行时日志静默：分发构建避免泄露模型网络结构 ── */
static void mlvc_quiet_runtime(void)
{
#if defined(RKVC_MLVC_TESTING)
    return; /* fake runtime 无日志 */
#else
    setenv("RKNN_LOG_LEVEL", "0", 1);
#endif
}

/** 多核 NPU：向下尝试 core_mask，驱动为最终权威。 */
static int mlvc_apply_npu_cores(rknn_context ctx)
{
    int cores = 3;
    while (cores > 1) {
        rknn_core_mask mask = cores == 3 ? RKNN_NPU_CORE_0_1_2
                                         : RKNN_NPU_CORE_0_1;
        if (rknn_set_core_mask(ctx, mask) == RKNN_SUCC)
            return cores;
        cores--;
    }
    return 0;
}

#define MLVC_MAX_IO 8
#define MLVC_MAX_MODEL_BYTES ((size_t)256 * 1024 * 1024)

static void push_reason(rkvc_diag **diag, rkvc_status status,
                        const rkvc_node *node, const char *reason) {
    if (diag)
        rkvc_diag_push(diag, status, 3,
                       node && node->ops ? node->ops->id : "mlvc",
                       reason);
}

/* ════════════════════════════════════════════════════════════════════ */
/*  RKNN 模型封装（标准 I/O + native 零拷贝 I/O）                       */
/* ════════════════════════════════════════════════════════════════════ */

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr in_attr[MLVC_MAX_IO];
    rknn_tensor_attr out_attr[MLVC_MAX_IO];
    rknn_tensor_attr native_out_attr[MLVC_MAX_IO];
    rknn_tensor_attr in_mem_attr[MLVC_MAX_IO]; /* 实际 set_io_mem 用的 attr */
    rknn_tensor_mem *in_mem[MLVC_MAX_IO];
    rknn_tensor_mem *out_mem[MLVC_MAX_IO];
} mlvc_rknn_model;

static void rknn_model_cleanup(mlvc_rknn_model *m)
{
    if (!m)
        return;
    uint32_t ni = m->io_num.n_input;
    uint32_t no = m->io_num.n_output;
    if (ni > MLVC_MAX_IO)
        ni = MLVC_MAX_IO;
    if (no > MLVC_MAX_IO)
        no = MLVC_MAX_IO;
    for (uint32_t i = 0; i < ni; i++)
        if (m->in_mem[i])
            rknn_destroy_mem(m->ctx, m->in_mem[i]);
    for (uint32_t i = 0; i < no; i++)
        if (m->out_mem[i])
            rknn_destroy_mem(m->ctx, m->out_mem[i]);
    if (m->ctx)
        rknn_destroy(m->ctx);
    memset(m, 0, sizeof(*m));
}

/**
 * 初始化 RKNN 模型：应用 QPPATCH（可选拷贝）→ rknn_init → 属性查询。
 * model 字节为图持有只读载荷；带补丁时拷出可写副本。
 */
static int rknn_model_init(mlvc_rknn_model *m, const unsigned char *model,
                           size_t model_size, const uint8_t *patch,
                           size_t patch_size, int qp)
{
    unsigned char *writable = NULL;
    int rc;

    memset(m, 0, sizeof(*m));
    if (!model || !model_size || model_size > MLVC_MAX_MODEL_BYTES)
        return (int)RKVC_STATUS_FORMAT;

    if (patch && patch_size) {
        writable = malloc(model_size);
        if (!writable)
            return (int)RKVC_STATUS_NOMEM;
        memcpy(writable, model, model_size);
        rc = mlvc_qppatch_apply(writable, model_size, patch, patch_size, qp);
        if (rc != 0) {
            free(writable);
            return rc;
        }
        model = writable;
    }

    mlvc_quiet_runtime();
    rc = rknn_init(&m->ctx, (void *)model, (uint32_t)model_size, 0, NULL);
    if (rc != RKNN_SUCC) {
        free(writable);
        return (int)RKVC_STATUS_HW;
    }
    (void)mlvc_apply_npu_cores(m->ctx);

    rc = rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io_num,
                    sizeof(m->io_num));
    if (rc != RKNN_SUCC || m->io_num.n_input > MLVC_MAX_IO ||
        m->io_num.n_output > MLVC_MAX_IO)
        goto fail;

    for (uint32_t i = 0; i < m->io_num.n_input; i++) {
        m->in_attr[i].index = i;
        rc = rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &m->in_attr[i],
                        sizeof(m->in_attr[i]));
        if (rc != RKNN_SUCC)
            goto fail;
    }
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        m->out_attr[i].index = i;
        rc = rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &m->out_attr[i],
                        sizeof(m->out_attr[i]));
        if (rc != RKNN_SUCC)
            goto fail;
    }
    free(writable);
    return 0;

fail:
    rknn_model_cleanup(m);
    free(writable);
    return (int)RKVC_STATUS_FORMAT;
}

static int rknn_find_input(const mlvc_rknn_model *m, const char *key)
{
    for (uint32_t i = 0; i < m->io_num.n_input; i++)
        if (strstr(m->in_attr[i].name, key))
            return (int)i;
    return -1;
}

static int rknn_find_output(const mlvc_rknn_model *m, const char *key)
{
    for (uint32_t i = 0; i < m->io_num.n_output; i++)
        if (strstr(m->out_attr[i].name, key))
            return (int)i;
    return -1;
}

static int tensor_is_pixel_input(const rknn_tensor_attr *a)
{
    if (!a || a->n_dims != 4)
        return 0;
    uint32_t d1 = a->dims[1], d2 = a->dims[2], d3 = a->dims[3];
    if (d3 == 3 && d1 >= 64 && d2 >= 64)
        return 1; /* NHWC 图像 */
    if (d1 == 3 && d2 >= 64 && d3 >= 64)
        return 1; /* NCHW 图像 */
    return 0;
}

static int rknn_query_input_mem_attr(mlvc_rknn_model *m, uint32_t i,
                                     rknn_tensor_attr *out)
{
    if (!m || !out || i >= m->io_num.n_input)
        return 0;
    memset(out, 0, sizeof(*out));
    out->index = i;
    if (tensor_is_pixel_input(&m->in_attr[i])) {
        int q = rknn_query(m->ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR,
                           out, sizeof(*out));
        if (q == RKNN_SUCC && out->n_dims == 4 && out->n_elems > 0)
            return 1;
    } else {
        int q = rknn_query(m->ctx, RKNN_QUERY_NATIVE_NC1HWC2_INPUT_ATTR,
                           out, sizeof(*out));
        if (q == RKNN_SUCC && out->n_dims >= 5 && out->n_elems > 0)
            return 1;
    }
    *out = m->in_attr[i];
    out->index = i;
    out->type = RKNN_TENSOR_FLOAT16;
    out->fmt = RKNN_TENSOR_NHWC;
    return 0;
}

static int rknn_alloc_input_mems(mlvc_rknn_model *m)
{
    for (uint32_t i = 0; i < m->io_num.n_input; i++) {
        rknn_tensor_attr a;
        (void)rknn_query_input_mem_attr(m, i, &a);
        a.pass_through = 1;
        uint32_t bytes = a.size_with_stride
            ? a.size_with_stride
            : a.n_elems * (uint32_t)sizeof(uint16_t);
        a.size = bytes;
        m->in_mem_attr[i] = a;
        m->in_mem[i] = rknn_create_mem(m->ctx, bytes);
        if (!m->in_mem[i])
            return (int)RKVC_STATUS_HW;
        if (rknn_set_io_mem(m->ctx, m->in_mem[i], &a) != RKNN_SUCC)
            return (int)RKVC_STATUS_HW;
    }
    return 0;
}

static int rknn_alloc_output_mems(mlvc_rknn_model *m)
{
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        if (rknn_query(m->ctx, RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR,
                       &a, sizeof(a)) != RKNN_SUCC)
            return (int)RKVC_STATUS_HW;
        a.index = i;
        a.pass_through = 1;
        m->native_out_attr[i] = a;
        m->out_mem[i] = rknn_create_mem(m->ctx, a.size_with_stride);
        if (!m->out_mem[i])
            return (int)RKVC_STATUS_HW;
        if (rknn_set_io_mem(m->ctx, m->out_mem[i], &a) != RKNN_SUCC)
            return (int)RKVC_STATUS_HW;
    }
    return 0;
}

static const uint16_t *rknn_output_data(mlvc_rknn_model *m, int i)
{
    if (!m || i < 0 || (uint32_t)i >= m->io_num.n_output || !m->out_mem[i])
        return NULL;
    rknn_mem_sync(m->ctx, m->out_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
    return (const uint16_t *)m->out_mem[i]->virt_addr;
}

static void rknn_write_input(mlvc_rknn_model *m, int i, const uint16_t *data)
{
    if (!m || i < 0 || (uint32_t)i >= m->io_num.n_input || !m->in_mem[i] ||
        !data)
        return;
    rknn_tensor_attr *a = &m->in_mem_attr[i];
    size_t nbytes = (size_t)a->n_elems * sizeof(uint16_t);
    if (a->fmt == RKNN_TENSOR_NHWC && a->n_dims == 4 &&
        a->w_stride != 0 && a->w_stride != a->dims[2]) {
        int H = (int)a->dims[1], W = (int)a->dims[2], C = (int)a->dims[3];
        uint16_t *dst = (uint16_t *)m->in_mem[i]->virt_addr;
        for (int h = 0; h < H; h++)
            memcpy(dst + (size_t)h * a->w_stride * C,
                   data + (size_t)h * W * C,
                   (size_t)W * C * sizeof(uint16_t));
    } else {
        memcpy(m->in_mem[i]->virt_addr, data, nbytes);
    }
    rknn_mem_sync(m->ctx, m->in_mem[i], RKNN_MEMORY_SYNC_TO_DEVICE);
}

/* ── fp16 辅助 ── */

static int fp16_tensor_is_finite(const uint16_t *data, size_t n)
{
    if (!data)
        return 0;
    for (size_t i = 0; i < n; i++) {
        if ((data[i] & 0x7c00u) == 0x7c00u)
            return 0;
    }
    return 1;
}

static void nchw_fp16_to_int32(const uint16_t *src, int32_t *dst, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = (int32_t)lrintf(mlvc_px_f16_to_f32(src[i]));
}

static void nchw_to_nhwc_fp16(const uint16_t *src, uint16_t *dst,
                              int C, int H, int W)
{
    for (int h = 0; h < H; h++) {
        for (int w = 0; w < W; w++) {
            uint16_t *dp = dst + ((size_t)h * W + w) * C;
            for (int c = 0; c < C; c++)
                dp[c] = src[((size_t)c * H + h) * W + w];
        }
    }
}

/* ── 模型绑定的公共部分：PMF → rANS coder ── */

/** 按 kind 在绑定载荷视图中查找（兼容旧 PMF kind=2 无区分的容器）。 */
static const rkvc_model_payload_view *find_payload(
    const rkvc_model_binding *binding, uint32_t kind)
{
    size_t i;
    if (!binding)
        return NULL;
    for (i = 0; i < binding->payload_count; ++i)
        if (binding->payloads[i].kind == kind)
            return &binding->payloads[i];
    return NULL;
}

/* QPP1 头（48B 小端）内联读取 qp（offset 16）；不完整/非 QPP1 返回 -1。 */
static int qppatch_qp(const rkvc_model_payload_view *v)
{
    const uint8_t *p;
    if (!v || !v->data || v->size < MLVC_QPPATCH_HEADER_SIZE)
        return -1;
    p = v->data;
    if (memcmp(p, MLVC_QPPATCH_MAGIC, 4) != 0)
        return -1;
    return (int)((uint32_t)p[16] | ((uint32_t)p[17] << 8) |
                 ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24));
}

/**
 * 多 QP 单模型：容器可携带多个 QPPATCH 载荷（每 qp 一份，基座 qp 的
 * 空补丁也含在内）。按目标 qp 选择；仅一个补丁且 qp 匹配时沿用。
 */
static const rkvc_model_payload_view *find_qppatch_for_qp(
    const rkvc_model_binding *binding, int qp)
{
    const rkvc_model_payload_view *first = NULL;
    const rkvc_model_payload_view *hit = NULL;
    size_t count = 0;
    size_t i;

    if (!binding)
        return NULL;
    for (i = 0; i < binding->payload_count; ++i) {
        const rkvc_model_payload_view *v = &binding->payloads[i];
        if (v->kind != RKMODEL_PAYLOAD_QPPATCH)
            continue;
        if (!first)
            first = v;
        if (!hit && qppatch_qp(v) == qp)
            hit = v;
        count++;
    }
    if (!count)
        return NULL;
    if (hit)
        return hit;
    /* 单补丁且声明 qp 与目标一致（或无法解析）时交由 apply 校验。 */
    if (count == 1 && qppatch_qp(first) < 0)
        return first;
    return NULL;
}

static int init_rans_from_binding(const rkvc_model_binding *binding,
                                  rkvc_rans_coder *g, rkvc_rans_coder *b,
                                  int require_gaussian_index_space)
{
    const rkvc_model_payload_view *gv, *bv;
    mlvc_pmf gpmf, bpmf;
    int rc;

    gv = find_payload(binding, RKMODEL_PAYLOAD_PMF_GAUSSIAN);
    if (!gv)
        gv = find_payload(binding, RKMODEL_PAYLOAD_PMF);
    bv = find_payload(binding, RKMODEL_PAYLOAD_PMF_BITEST);
    if (!gv || !bv)
        return (int)RKVC_STATUS_FORMAT;

    rc = mlvc_pmf_load(gv->data, gv->size, &gpmf);
    if (rc != 0)
        return rc;
    rc = mlvc_pmf_load(bv->data, bv->size, &bpmf);
    if (rc != 0) {
        mlvc_pmf_free(&gpmf);
        return rc;
    }
    if (require_gaussian_index_space && !gpmf.index_space) {
        mlvc_pmf_free(&gpmf);
        mlvc_pmf_free(&bpmf);
        return (int)RKVC_STATUS_FORMAT;
    }

    rc = rkvc_rans_coder_init(g, RKVC_RANS_BYTE, gpmf.lengths,
                              gpmf.num_lengths, gpmf.offsets,
                              gpmf.num_offsets, gpmf.table, gpmf.num_table,
                              16, 2);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_coder_init(b, RKVC_RANS_BYTE, bpmf.lengths,
                                  bpmf.num_lengths, bpmf.offsets,
                                  bpmf.num_offsets, bpmf.table, bpmf.num_table,
                                  16, 2);
    mlvc_pmf_free(&gpmf);
    mlvc_pmf_free(&bpmf);
    return rc == RKVC_RANS_OK ? 0 : (int)RKVC_STATUS_FORMAT;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  MLVC 编码器节点                                                      */
/* ════════════════════════════════════════════════════════════════════ */

struct mlvc_encoder {
    rkvc_request request;
    mlvc_rknn_model enc_model;
    rkvc_rans_coder g_coder, b_coder;
    int rans_ready;
    int qp;

    int IMG_W, IMG_H, REF_C, REF_H, REF_W;
    int ZC, ZH, ZW, YC, YH, YW;
    int enc_x_in, enc_ref_in;
    int enc_feat_out, enc_z_out, enc_y0_out, enc_y1_out;
    int zero_copy_inputs;
    int zero_copy_x_direct;

    uint16_t *x_nhwc;
    uint16_t *ref_nhwc;
    int32_t *z_r, *y0_r, *y1_r, *s0, *s1, *z_idx;

    uint32_t frame_count;
};

static int enc_resolve_geom(struct mlvc_encoder *e)
{
    mlvc_rknn_model *enc = &e->enc_model;
    e->enc_x_in = rknn_find_input(enc, "x");
    if (e->enc_x_in < 0)
        e->enc_x_in = rknn_find_input(enc, "New_input_x");
    e->enc_ref_in = rknn_find_input(enc, "ref_feature");
    if (e->enc_x_in < 0 || e->enc_ref_in < 0)
        return (int)RKVC_STATUS_FORMAT;

    e->IMG_H = (int)enc->in_attr[e->enc_x_in].dims[1];
    e->IMG_W = (int)enc->in_attr[e->enc_x_in].dims[2];
    e->REF_H = (int)enc->in_attr[e->enc_ref_in].dims[1];
    e->REF_W = (int)enc->in_attr[e->enc_ref_in].dims[2];
    e->REF_C = (int)enc->in_attr[e->enc_ref_in].dims[3];

    e->enc_feat_out = rknn_find_output(enc, "feature");
    e->enc_z_out = rknn_find_output(enc, "z_raw");
    e->enc_y0_out = rknn_find_output(enc, "y_raw_0");
    e->enc_y1_out = rknn_find_output(enc, "y_raw_1");
    if (e->enc_feat_out < 0 || e->enc_z_out < 0 ||
        e->enc_y0_out < 0 || e->enc_y1_out < 0)
        return (int)RKVC_STATUS_FORMAT;
    if (enc->in_attr[e->enc_x_in].type != RKNN_TENSOR_FLOAT16 ||
        enc->in_attr[e->enc_ref_in].type != RKNN_TENSOR_FLOAT16 ||
        enc->out_attr[e->enc_feat_out].type != RKNN_TENSOR_FLOAT16 ||
        enc->out_attr[e->enc_z_out].type != RKNN_TENSOR_FLOAT16 ||
        enc->out_attr[e->enc_y0_out].type != RKNN_TENSOR_FLOAT16 ||
        enc->out_attr[e->enc_y1_out].type != RKNN_TENSOR_FLOAT16)
        return (int)RKVC_STATUS_FORMAT;

    /* encoder 标准输出 API 返回逻辑 NCHW。 */
    const rknn_tensor_attr *za = &enc->out_attr[e->enc_z_out];
    const rknn_tensor_attr *ya = &enc->out_attr[e->enc_y0_out];
    if (za->n_dims < 4 || ya->n_dims < 4)
        return (int)RKVC_STATUS_FORMAT;
    e->ZC = (int)za->dims[1];
    e->ZH = (int)za->dims[2];
    e->ZW = (int)za->dims[3];
    e->YC = (int)ya->dims[1];
    e->YH = (int)ya->dims[2];
    e->YW = (int)ya->dims[3];
    const rknn_tensor_attr *fa = &enc->out_attr[e->enc_feat_out];
    if (fa->n_dims < 4 || fa->dims[1] != (uint32_t)e->REF_C ||
        fa->dims[2] != (uint32_t)e->REF_H ||
        fa->dims[3] != (uint32_t)e->REF_W ||
        fa->n_elems != enc->in_attr[e->enc_ref_in].n_elems)
        return (int)RKVC_STATUS_FORMAT;
    return 0;
}

static int enc_zero_copy_requested(void)
{
    const char *v = getenv("RKVC_MLVC_ENCODER_ZERO_COPY");
    return !v || !v[0] || strcmp(v, "0") != 0;
}

static int enc_native_x_compatible(const struct mlvc_encoder *e,
                                   const rknn_tensor_attr *a)
{
    if (!e || !a || a->n_dims != 4 || a->type != RKNN_TENSOR_FLOAT16 ||
        a->fmt != RKNN_TENSOR_NHWC)
        return 0;
    uint32_t h = a->dims[1], w = a->dims[2], c = a->dims[3];
    uint32_t ws = a->w_stride ? a->w_stride : w;
    size_t required = (size_t)h * ws * c * sizeof(uint16_t);
    size_t available = a->size_with_stride
        ? a->size_with_stride
        : (size_t)a->n_elems * sizeof(uint16_t);
    return h == (uint32_t)e->IMG_H && w == (uint32_t)e->IMG_W && c == 3 &&
           ws >= w && required <= available;
}

static int enc_native_ref_compatible(const struct mlvc_encoder *e,
                                     const rknn_tensor_attr *a)
{
    if (!e || !a || a->n_dims < 5 || a->type != RKNN_TENSOR_FLOAT16)
        return 0;
    uint32_t c1 = a->dims[1], h = a->dims[2], w = a->dims[3], c2 = a->dims[4];
    uint32_t ws = a->w_stride ? a->w_stride : w;
    if (!c1 || !c2 || h != (uint32_t)e->REF_H || w != (uint32_t)e->REF_W ||
        c1 * c2 < (uint32_t)e->REF_C || ws < w)
        return 0;
    size_t required = (size_t)c1 * h * ws * c2 * sizeof(uint16_t);
    size_t available = a->size_with_stride
        ? a->size_with_stride
        : (size_t)a->n_elems * sizeof(uint16_t);
    return required <= available;
}

static int enc_init_zero_copy_inputs(struct mlvc_encoder *e)
{
    if (!enc_zero_copy_requested())
        return 0;

    rknn_tensor_attr x_attr, ref_attr;
    int native_x = rknn_query_input_mem_attr(&e->enc_model,
                                             (uint32_t)e->enc_x_in, &x_attr);
    int native = rknn_query_input_mem_attr(&e->enc_model,
                                           (uint32_t)e->enc_ref_in, &ref_attr);
    if (!native_x || !enc_native_x_compatible(e, &x_attr) ||
        !native || !enc_native_ref_compatible(e, &ref_attr))
        return 0; /* 回退标准 host 输入 */

    int rc = rknn_alloc_input_mems(&e->enc_model);
    if (rc != 0)
        return rc;
    e->zero_copy_inputs = 1;

    const rknn_tensor_attr *bound_x = &e->enc_model.in_mem_attr[e->enc_x_in];
    uint32_t x_ws = bound_x->w_stride ? bound_x->w_stride : bound_x->dims[2];
    e->zero_copy_x_direct =
        bound_x->n_dims == 4 && bound_x->fmt == RKNN_TENSOR_NHWC &&
        bound_x->dims[1] == (uint32_t)e->IMG_H &&
        bound_x->dims[2] == (uint32_t)e->IMG_W && bound_x->dims[3] == 3 &&
        x_ws == (uint32_t)e->IMG_W;

    rknn_tensor_mem *ref_mem = e->enc_model.in_mem[e->enc_ref_in];
    size_t bytes = e->enc_model.in_mem_attr[e->enc_ref_in].size_with_stride;
    if (!bytes)
        bytes = (size_t)e->enc_model.in_mem_attr[e->enc_ref_in].n_elems *
                sizeof(uint16_t);
    memset(ref_mem->virt_addr, 0, bytes);
    if (rknn_mem_sync(e->enc_model.ctx, ref_mem,
                      RKNN_MEMORY_SYNC_TO_DEVICE) != RKNN_SUCC)
        return (int)RKVC_STATUS_HW;
    return 0;
}

static int enc_alloc_bufs(struct mlvc_encoder *e)
{
    size_t img_sz = (size_t)e->IMG_H * e->IMG_W;
    size_t ref_sz = (size_t)e->REF_C * e->REF_H * e->REF_W;
    size_t z_sz = (size_t)e->ZC * e->ZH * e->ZW;
    size_t y_sz = (size_t)e->YC * e->YH * e->YW;

    if (!e->zero_copy_x_direct) {
        e->x_nhwc = malloc(img_sz * 3 * 2);
        if (!e->x_nhwc)
            return (int)RKVC_STATUS_NOMEM;
    }
    if (!e->zero_copy_inputs) {
        e->ref_nhwc = malloc(ref_sz * 2);
        if (!e->ref_nhwc)
            return (int)RKVC_STATUS_NOMEM;
        memset(e->ref_nhwc, 0, ref_sz * 2);
    }
    e->z_r = malloc(z_sz * 4);
    e->y0_r = malloc(y_sz * 4);
    e->y1_r = malloc(y_sz * 4);
    e->s0 = malloc(y_sz * 4);
    e->s1 = malloc(y_sz * 4);
    e->z_idx = malloc(z_sz * 4);
    if (!e->z_r || !e->y0_r || !e->y1_r || !e->s0 || !e->s1 || !e->z_idx)
        return (int)RKVC_STATUS_NOMEM;

    /* z_idx 在通道平面内为常量：z_idx = qp*ZC + c。 */
    for (int c = 0; c < e->ZC; c++) {
        int32_t *p = e->z_idx + (size_t)c * e->ZH * e->ZW;
        size_t n = (size_t)e->ZH * e->ZW;
        for (size_t i = 0; i < n; i++)
            p[i] = e->qp * e->ZC + c;
    }
    return 0;
}

static void enc_free_bufs(struct mlvc_encoder *e)
{
    free(e->x_nhwc);
    free(e->ref_nhwc);
    free(e->z_r);
    free(e->y0_r);
    free(e->y1_r);
    free(e->s0);
    free(e->s1);
    free(e->z_idx);
    e->x_nhwc = NULL;
    e->ref_nhwc = NULL;
    e->z_r = e->y0_r = e->y1_r = e->s0 = e->s1 = e->z_idx = NULL;
}

static int mlvc_enc_bind_model(rkvc_node *node,
                               const rkvc_model_binding *binding,
                               rkvc_diag **diag)
{
    struct mlvc_encoder *e = node ? node->priv : NULL;
    int rc;

    if (!e || !binding || !binding->info || !binding->payload ||
        !binding->payload_size) {
        push_reason(diag, RKVC_STATUS_FORMAT, node, "missing model binding");
        return (int)RKVC_STATUS_FORMAT;
    }
    if (strcmp(binding->info->role, "encoder") != 0) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "model role is not encoder");
        return (int)RKVC_STATUS_FORMAT;
    }

    e->qp = e->request.quality.qp >= 0 ? (int)e->request.quality.qp : 21;
    if (e->qp > 63) {
        push_reason(diag, RKVC_STATUS_INVALID, node, "mlvc qp out of range");
        return (int)RKVC_STATUS_INVALID;
    }

    rc = init_rans_from_binding(binding, &e->g_coder, &e->b_coder, 1);
    if (rc != 0) {
        push_reason(diag, (rkvc_status)rc, node, "PMF/rANS init failed");
        return rc;
    }
    e->rans_ready = 1;

    const rkvc_model_payload_view *patch =
        find_qppatch_for_qp(binding, e->qp);
    rc = rknn_model_init(&e->enc_model, binding->payload,
                         binding->payload_size,
                         patch ? patch->data : NULL,
                         patch ? patch->size : 0, e->qp);
    if (rc != 0) {
        push_reason(diag, (rkvc_status)rc, node,
                    "encoder RKNN init failed (qp patch/model)");
        return rc;
    }
    return 0;
}

static int mlvc_enc_configure(rkvc_node *node, rkvc_diag **diag)
{
    rkvc_frame_spec in = {0};
    rkvc_frame_spec out = {0};
    (void)diag;
    in.fmt = RKVC_FRAME_FMT_NV12;
    in.domain = RKVC_MEM_DOMAIN_HOST;
    out.fmt = RKVC_FRAME_FMT_BITSTREAM;
    out.domain = RKVC_MEM_DOMAIN_HOST;
    rkvc_port_set_desired(&node->in_ports[0], &in);
    rkvc_port_set_desired(&node->out_ports[0], &out);
    return 0;
}

static int mlvc_enc_open(rkvc_node *node, rkvc_diag **diag)
{
    struct mlvc_encoder *e = node->priv;
    int rc;

    if (!e->rans_ready) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "encoder opened without a bound model");
        return (int)RKVC_STATUS_FORMAT;
    }
    rc = enc_resolve_geom(e);
    if (rc != 0) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "encoder tensor contract mismatch");
        return rc;
    }
    rc = enc_init_zero_copy_inputs(e);
    if (rc != 0)
        return rc;
    rc = enc_alloc_bufs(e);
    if (rc != 0)
        return rc;

    if ((e->request.width && e->request.width != (uint32_t)e->IMG_W) ||
        (e->request.height && e->request.height != (uint32_t)e->IMG_H)) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "request geometry does not match encoder model");
        return (int)RKVC_STATUS_FORMAT;
    }
    return 0;
}

static void free_buffer_fn(void *ptr) { free(ptr); }

/** 编码一帧 NV12 → 一条 .mlvc 记录的 BITSTREAM 帧并发出。 */
static int mlvc_enc_process(rkvc_node *node, rkvc_frame *input,
                            rkvc_diag **diag)
{
    struct mlvc_encoder *e = node->priv;
    rkvc_frame_desc desc;
    const uint8_t *base;
    uint16_t *x_dst;
    uint64_t profile_unused;
    rknn_output outputs[MLVC_MAX_IO];
    rkvc_rans_enc_stream stream;
    const uint8_t *bits;
    size_t bits_size, record_size;
    uint8_t *record;
    rkvc_frame_desc out_desc;
    rkvc_frame *output = NULL;
    rkvc_status st;
    int rc;
    uint32_t i;
    int keyframe;

    if (!input || rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        desc.spec.fmt != RKVC_FRAME_FMT_NV12 ||
        desc.spec.domain != RKVC_MEM_DOMAIN_HOST)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.width != (uint32_t)e->IMG_W ||
        desc.spec.height != (uint32_t)e->IMG_H)
        return (int)RKVC_STATUS_FORMAT;
    if (!desc.data)
        return (int)RKVC_STATUS_FORMAT;
    base = desc.data;

    /* NV12 → NHWC fp16（native 直写或暂存缓冲） */
    x_dst = e->zero_copy_x_direct
        ? (uint16_t *)e->enc_model.in_mem[e->enc_x_in]->virt_addr
        : e->x_nhwc;
    {
        uint32_t stride = desc.spec.stride ? desc.spec.stride
                                           : desc.spec.width;
        uint32_t vstride = desc.spec.ver_stride ? desc.spec.ver_stride
                                                : desc.spec.height;
        const uint8_t *uv = base + (size_t)stride * vstride;
        mlvc_px_yuv_to_nhwc_fp16(base, (int)stride, uv, uv + 1,
                                 (int)stride, 1, e->IMG_W, e->IMG_H, x_dst);
    }

    /* encoder NPU（混合 I/O：零拷贝输入 + 逻辑输出） */
    if (e->zero_copy_inputs) {
        if (e->zero_copy_x_direct) {
            if (rknn_mem_sync(e->enc_model.ctx,
                              e->enc_model.in_mem[e->enc_x_in],
                              RKNN_MEMORY_SYNC_TO_DEVICE) != RKNN_SUCC)
                return (int)RKVC_STATUS_HW;
        } else {
            rknn_write_input(&e->enc_model, e->enc_x_in, e->x_nhwc);
        }
    } else {
        rknn_input inputs[MLVC_MAX_IO];
        memset(inputs, 0, sizeof(inputs));
        inputs[e->enc_x_in].index = (uint32_t)e->enc_x_in;
        inputs[e->enc_x_in].buf = e->x_nhwc;
        inputs[e->enc_x_in].size =
            e->enc_model.in_attr[e->enc_x_in].n_elems * sizeof(uint16_t);
        inputs[e->enc_x_in].type = RKNN_TENSOR_FLOAT16;
        inputs[e->enc_x_in].fmt = RKNN_TENSOR_NHWC;
        inputs[e->enc_ref_in].index = (uint32_t)e->enc_ref_in;
        inputs[e->enc_ref_in].buf = e->ref_nhwc;
        inputs[e->enc_ref_in].size =
            e->enc_model.in_attr[e->enc_ref_in].n_elems * sizeof(uint16_t);
        inputs[e->enc_ref_in].type = RKNN_TENSOR_FLOAT16;
        inputs[e->enc_ref_in].fmt = RKNN_TENSOR_NHWC;
        if (rknn_inputs_set(e->enc_model.ctx, e->enc_model.io_num.n_input,
                            inputs) != RKNN_SUCC)
            return (int)RKVC_STATUS_HW;
    }
    if (rknn_run(e->enc_model.ctx, NULL) != RKNN_SUCC) {
        push_reason(diag, RKVC_STATUS_HW, node, "encoder RKNN run failed");
        return (int)RKVC_STATUS_HW;
    }

    memset(outputs, 0, sizeof(outputs));
    for (i = 0; i < e->enc_model.io_num.n_output; i++) {
        outputs[i].index = i;
        outputs[i].want_float = 0;
        outputs[i].is_prealloc = 0;
    }
    if (rknn_outputs_get(e->enc_model.ctx, e->enc_model.io_num.n_output,
                         outputs, NULL) != RKNN_SUCC)
        return (int)RKVC_STATUS_HW;
    for (i = 0; i < e->enc_model.io_num.n_output; i++) {
        size_t required =
            (size_t)e->enc_model.out_attr[i].n_elems * sizeof(uint16_t);
        if (!outputs[i].buf || outputs[i].size < required) {
            rknn_outputs_release(e->enc_model.ctx,
                                 e->enc_model.io_num.n_output, outputs);
            return (int)RKVC_STATUS_HW;
        }
    }
    const uint16_t *bz = outputs[e->enc_z_out].buf;
    const uint16_t *by0 = outputs[e->enc_y0_out].buf;
    const uint16_t *by1 = outputs[e->enc_y1_out].buf;
    const uint16_t *fv = outputs[e->enc_feat_out].buf;
    if (!fp16_tensor_is_finite(bz,
            e->enc_model.out_attr[e->enc_z_out].n_elems) ||
        !fp16_tensor_is_finite(by0,
            e->enc_model.out_attr[e->enc_y0_out].n_elems) ||
        !fp16_tensor_is_finite(by1,
            e->enc_model.out_attr[e->enc_y1_out].n_elems) ||
        !fp16_tensor_is_finite(fv,
            e->enc_model.out_attr[e->enc_feat_out].n_elems)) {
        rknn_outputs_release(e->enc_model.ctx,
                             e->enc_model.io_num.n_output, outputs);
        push_reason(diag, RKVC_STATUS_HW, node,
                    "encoder produced non-finite latent values");
        return (int)RKVC_STATUS_HW;
    }

    nchw_fp16_to_int32(bz, e->z_r, (size_t)e->ZC * e->ZH * e->ZW);
    nchw_fp16_to_int32(by0, e->y0_r, (size_t)e->YC * e->YH * e->YW);
    nchw_fp16_to_int32(by1, e->y1_r, (size_t)e->YC * e->YH * e->YW);
    /* feature 输出 → 下一帧参考（native NC1HWC2 或逻辑 NHWC） */
    if (e->zero_copy_inputs) {
        rknn_tensor_attr *a = &e->enc_model.in_mem_attr[e->enc_ref_in];
        uint16_t *dst = (uint16_t *)e->enc_model.in_mem[e->enc_ref_in]
                            ->virt_addr;
        int c2 = (int)a->dims[4];
        int ws = a->w_stride ? (int)a->w_stride : (int)a->dims[3];
        mlvc_px_nchw_f16_to_nc1hwc2(fv, dst, e->REF_C, e->REF_H,
                                    e->REF_W, c2, ws);
        if (rknn_mem_sync(e->enc_model.ctx,
                          e->enc_model.in_mem[e->enc_ref_in],
                          RKNN_MEMORY_SYNC_TO_DEVICE) != RKNN_SUCC) {
            rknn_outputs_release(e->enc_model.ctx,
                                 e->enc_model.io_num.n_output, outputs);
            return (int)RKVC_STATUS_HW;
        }
    } else {
        nchw_to_nhwc_fp16(fv, e->ref_nhwc, e->REF_C, e->REF_H, e->REF_W);
    }
    rknn_outputs_release(e->enc_model.ctx, e->enc_model.io_num.n_output,
                         outputs);
    (void)profile_unused;

    /* 熵编码：y1, y0, z 三段流 */
    mlvc_px_extract_scales(e->z_r, e->s0, e->s1, e->YC, e->YH, e->YW,
                           e->ZH, e->ZW);
    rkvc_rans_enc_stream_init(&stream, RKVC_RANS_BYTE, 65536);
    rc = rkvc_rans_enc_stream_encode(&stream, &e->g_coder, e->s1, e->y1_r,
                                     (size_t)e->YC * e->YH * e->YW);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_enc_stream_encode(&stream, &e->g_coder, e->s0,
                                         e->y0_r,
                                         (size_t)e->YC * e->YH * e->YW);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_enc_stream_encode(&stream, &e->b_coder, e->z_idx,
                                         e->z_r,
                                         (size_t)e->ZC * e->ZH * e->ZW);
    if (rc != RKVC_RANS_OK) {
        rkvc_rans_enc_stream_free(&stream);
        return (int)RKVC_STATUS_INTERNAL;
    }
    bits = rkvc_rans_enc_stream_flush(&stream, &bits_size);
    if (!bits) {
        rkvc_rans_enc_stream_free(&stream);
        return (int)RKVC_STATUS_INTERNAL;
    }

    /* 组装 .mlvc 记录（首帧附 32B 容器头） */
    keyframe = e->frame_count == 0;
    record_size = (keyframe ? MLVC_HDR_SIZE : 0) + MLVC_REC_SIZE + bits_size;
    record = malloc(record_size);
    if (!record) {
        rkvc_rans_enc_stream_free(&stream);
        return (int)RKVC_STATUS_NOMEM;
    }
    {
        uint8_t *cur = record;
        if (keyframe) {
            mlvc_container_write_header(cur, (uint32_t)e->IMG_W,
                                        (uint32_t)e->IMG_H, 0, 0,
                                        (uint32_t)e->qp);
            cur += MLVC_HDR_SIZE;
        }
        mlvc_container_write_record(cur, (uint32_t)bits_size, keyframe);
        cur += MLVC_REC_SIZE;
        memcpy(cur, bits, bits_size);
    }
    rkvc_rans_enc_stream_free(&stream);

    rkvc_frame_desc_init(&out_desc, sizeof(out_desc));
    out_desc.spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    out_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    out_desc.data = record;
    out_desc.size = record_size;
    out_desc.pts = desc.pts;
    out_desc.dts = desc.pts;
    if (keyframe)
        out_desc.flags |= RKVC_FRAME_FLAG_KEYFRAME;
    st = rkvc_backend_frame_create(&out_desc, free_buffer_fn, record,
                                   &output);
    if (st != RKVC_STATUS_OK) {
        free(record);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    e->frame_count++;
    return rc;
}

static void mlvc_enc_close(rkvc_node *node)
{
    struct mlvc_encoder *e = node ? node->priv : NULL;
    if (!e)
        return;
    enc_free_bufs(e);
    rknn_model_cleanup(&e->enc_model);
    if (e->rans_ready) {
        rkvc_rans_coder_free(&e->g_coder);
        rkvc_rans_coder_free(&e->b_coder);
        e->rans_ready = 0;
    }
}

static void mlvc_destroy_node(rkvc_node *node)
{
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops mlvc_enc_ops = {
    "mlvc.encode", mlvc_enc_configure, mlvc_enc_open, mlvc_enc_process,
    NULL, mlvc_enc_close, mlvc_destroy_node, mlvc_enc_bind_model,
};

/* ════════════════════════════════════════════════════════════════════ */
/*  MLVC 解码器节点                                                      */
/* ════════════════════════════════════════════════════════════════════ */

struct mlvc_decoder {
    rkvc_request request;
    mlvc_rknn_model dec_model;
    rkvc_rans_coder g_coder, b_coder;
    int rans_ready;
    int model_ready;
    int qp;

    int IMG_W, IMG_H, REF_C, REF_H, REF_W;
    int OUT_C2;
    int x_d2s_bs;
    int x_pre_c, x_pre_h, x_pre_w;
    int ZC, ZH, ZW, YC, YH, YW;
    int dec_z_in, dec_y0_in, dec_y1_in, dec_ref_in, dec_x_out, dec_ref_out;

    uint16_t *ref_nhwc;
    int32_t *z_d, *y0_d, *y1_d, *s0, *s1, *z_idx;
    uint16_t *z_d_nhwc, *y0_d_nhwc, *y1_d_nhwc;
    uint16_t *x_img;

    /* 绑定暂存：RKNN 惰性初始化（首帧容器头给出 qp 后） */
    const unsigned char *model_bytes;
    size_t model_size;
    rkvc_model_binding binding; /* 多 QP 补丁按 qp 选择用 */

    mlvc_demuxer demux;
    uint32_t frame_count;
};

static int dec_resolve_geom(struct mlvc_decoder *d)
{
    mlvc_rknn_model *dec = &d->dec_model;
    d->dec_z_in = rknn_find_input(dec, "z_raw");
    d->dec_y0_in = rknn_find_input(dec, "y_raw_0");
    d->dec_y1_in = rknn_find_input(dec, "y_raw_1");
    d->dec_ref_in = rknn_find_input(dec, "ref_feature");
    d->dec_x_out = rknn_find_output(dec, "x_hat");
    d->dec_ref_out = rknn_find_output(dec, "feature");
    if (d->dec_z_in < 0 || d->dec_y0_in < 0 || d->dec_y1_in < 0 ||
        d->dec_ref_in < 0 || d->dec_x_out < 0 || d->dec_ref_out < 0)
        return (int)RKVC_STATUS_FORMAT;

    int C1 = (int)dec->native_out_attr[d->dec_x_out].dims[1];
    int native_h = (int)dec->native_out_attr[d->dec_x_out].dims[2];
    int native_w = (int)dec->native_out_attr[d->dec_x_out].dims[3];
    d->OUT_C2 = (dec->native_out_attr[d->dec_x_out].n_dims >= 5)
        ? (int)dec->native_out_attr[d->dec_x_out].dims[4]
        : 8;
    if (d->OUT_C2 < 3)
        d->OUT_C2 = 3;
    d->x_pre_c = C1 * d->OUT_C2;
    d->x_pre_h = native_h;
    d->x_pre_w = native_w;
    d->x_d2s_bs = 0;
    d->IMG_H = native_h;
    d->IMG_W = native_w;
    if (d->x_pre_c % 3 == 0) {
        int pix = d->x_pre_c / 3;
        for (int s = 2; s <= 16; s++) {
            if (s * s == pix) {
                d->x_d2s_bs = s;
                d->IMG_H = native_h * s;
                d->IMG_W = native_w * s;
                break;
            }
        }
    }
    d->REF_C = (int)dec->in_attr[d->dec_ref_in].dims[3];
    d->REF_H = (int)dec->in_attr[d->dec_ref_in].dims[1];
    d->REF_W = (int)dec->in_attr[d->dec_ref_in].dims[2];

    d->ZC = (int)dec->in_attr[d->dec_z_in].dims[3];
    d->ZH = (int)dec->in_attr[d->dec_z_in].dims[1];
    d->ZW = (int)dec->in_attr[d->dec_z_in].dims[2];
    d->YC = (int)dec->in_attr[d->dec_y0_in].dims[3];
    d->YH = (int)dec->in_attr[d->dec_y0_in].dims[1];
    d->YW = (int)dec->in_attr[d->dec_y0_in].dims[2];
    if (dec->native_out_attr[d->dec_ref_out].n_elems !=
        dec->in_mem_attr[d->dec_ref_in].n_elems)
        return (int)RKVC_STATUS_FORMAT;
    return 0;
}

static int dec_alloc_bufs(struct mlvc_decoder *d)
{
    size_t ref_sz = (size_t)d->dec_model.in_mem_attr[d->dec_ref_in].n_elems;
    if (!ref_sz)
        ref_sz = (size_t)d->REF_C * d->REF_H * d->REF_W;
    size_t z_sz = (size_t)d->ZC * d->ZH * d->ZW;
    size_t y_sz = (size_t)d->YC * d->YH * d->YW;
    size_t z_in = (size_t)d->dec_model.in_mem_attr[d->dec_z_in].n_elems;
    size_t y_in = (size_t)d->dec_model.in_mem_attr[d->dec_y0_in].n_elems;
    if (z_in < z_sz)
        z_in = z_sz;
    if (y_in < y_sz)
        y_in = y_sz;

    d->ref_nhwc = malloc(ref_sz * 2);
    d->z_d = malloc(z_sz * 4);
    d->y0_d = malloc(y_sz * 4);
    d->y1_d = malloc(y_sz * 4);
    d->s0 = malloc(y_sz * 4);
    d->s1 = malloc(y_sz * 4);
    d->z_idx = malloc(z_sz * 4);
    d->z_d_nhwc = malloc(z_in * 2);
    d->y0_d_nhwc = malloc(y_in * 2);
    d->y1_d_nhwc = malloc(y_in * 2);
    d->x_img = NULL;
    if (d->x_d2s_bs > 0)
        d->x_img = malloc(3ull * d->IMG_H * d->IMG_W * 2);
    if (!d->ref_nhwc || !d->z_d || !d->y0_d || !d->y1_d || !d->s0 ||
        !d->s1 || !d->z_idx || !d->z_d_nhwc || !d->y0_d_nhwc ||
        !d->y1_d_nhwc || (d->x_d2s_bs > 0 && !d->x_img))
        return (int)RKVC_STATUS_NOMEM;

    memset(d->ref_nhwc, 0, ref_sz * 2);
    for (int c = 0; c < d->ZC; c++) {
        int32_t *p = d->z_idx + (size_t)c * d->ZH * d->ZW;
        size_t n = (size_t)d->ZH * d->ZW;
        for (size_t i = 0; i < n; i++)
            p[i] = d->qp * d->ZC + c;
    }
    return 0;
}

static void dec_free_bufs(struct mlvc_decoder *d)
{
    free(d->ref_nhwc);
    free(d->z_d);
    free(d->y0_d);
    free(d->y1_d);
    free(d->s0);
    free(d->s1);
    free(d->z_idx);
    free(d->z_d_nhwc);
    free(d->y0_d_nhwc);
    free(d->y1_d_nhwc);
    free(d->x_img);
    d->ref_nhwc = NULL;
    d->z_d = d->y0_d = d->y1_d = d->s0 = d->s1 = d->z_idx = NULL;
    d->z_d_nhwc = d->y0_d_nhwc = d->y1_d_nhwc = NULL;
    d->x_img = NULL;
}

static int mlvc_dec_bind_model(rkvc_node *node,
                               const rkvc_model_binding *binding,
                               rkvc_diag **diag)
{
    struct mlvc_decoder *d = node ? node->priv : NULL;
    int rc;

    if (!d || !binding || !binding->info || !binding->payload ||
        !binding->payload_size) {
        push_reason(diag, RKVC_STATUS_FORMAT, node, "missing model binding");
        return (int)RKVC_STATUS_FORMAT;
    }
    if (strcmp(binding->info->role, "decoder") != 0) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "model role is not decoder");
        return (int)RKVC_STATUS_FORMAT;
    }

    rc = init_rans_from_binding(binding, &d->g_coder, &d->b_coder, 1);
    if (rc != 0) {
        push_reason(diag, (rkvc_status)rc, node, "PMF/rANS init failed");
        return rc;
    }
    d->rans_ready = 1;

    /* 绑定暂存：多 QP 补丁须在容器头给出 qp 后才能选择。
     * binding 描述符由回调调用方在栈上构造，必须按值保存；其中的
     * info/payload/payloads 指针仍借用核心存储，在节点 destroy 前有效。 */
    d->model_bytes = binding->payload;
    d->model_size = binding->payload_size;
    d->binding = *binding;
    return 0;
}

static int mlvc_dec_configure(rkvc_node *node, rkvc_diag **diag)
{
    rkvc_frame_spec in = {0}; /* UNKNOWN：接受 source 的 BITSTREAM 块 */
    rkvc_frame_spec out = {0};
    (void)diag;
    out.fmt = RKVC_FRAME_FMT_NV12;
    out.domain = RKVC_MEM_DOMAIN_HOST;
    rkvc_port_set_desired(&node->in_ports[0], &in);
    rkvc_port_set_desired(&node->out_ports[0], &out);
    return 0;
}

static int mlvc_dec_open(rkvc_node *node, rkvc_diag **diag)
{
    struct mlvc_decoder *d = node->priv;
    (void)diag;
    if (!d->rans_ready || !d->model_bytes) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "decoder opened without a bound model");
        return (int)RKVC_STATUS_FORMAT;
    }
    mlvc_demux_init(&d->demux);
    return 0;
}

/** 首帧路径：容器头解析后用 qp 初始化 RKNN + 几何/缓冲。 */
static int dec_lazy_init(struct mlvc_decoder *d, rkvc_diag **diag,
                         const rkvc_node *node)
{
    const rkvc_model_payload_view *patch =
        find_qppatch_for_qp(&d->binding, d->qp);
    int rc = rknn_model_init(&d->dec_model, d->model_bytes, d->model_size,
                             patch ? patch->data : NULL,
                             patch ? patch->size : 0, d->qp);
    if (rc != 0) {
        push_reason(diag, (rkvc_status)rc, node,
                    "decoder RKNN init failed (qp patch/model)");
        return rc;
    }
    d->model_ready = 1;
    rc = rknn_alloc_input_mems(&d->dec_model);
    if (rc != 0)
        return rc;
    rc = rknn_alloc_output_mems(&d->dec_model);
    if (rc != 0)
        return rc;
    rc = dec_resolve_geom(d);
    if (rc != 0) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "decoder tensor contract mismatch");
        return rc;
    }
    return dec_alloc_bufs(d);
}

/** 解码一帧 rANS 载荷 → NV12 BITSTREAM→video 转换并发出。 */
static int dec_decode_frame(struct mlvc_decoder *d, rkvc_node *node,
                            const uint8_t *payload, size_t size,
                            int keyframe, rkvc_diag **diag)
{
    rkvc_rans_dec_stream ds;
    size_t z_n = (size_t)d->ZC * d->ZH * d->ZW;
    size_t y_n = (size_t)d->YC * d->YH * d->YW;
    const uint16_t *xh, *fv;
    uint8_t *nv12;
    size_t nv12_size;
    rkvc_frame_desc out_desc;
    rkvc_frame *output = NULL;
    rkvc_status st;
    int rc;

    rkvc_rans_dec_stream_init(&ds, RKVC_RANS_BYTE);
    if (rkvc_rans_dec_stream_open(&ds, payload, size) != RKVC_RANS_OK) {
        push_reason(diag, RKVC_STATUS_FORMAT, node, "rANS stream open failed");
        return (int)RKVC_STATUS_FORMAT;
    }
    rc = rkvc_rans_dec_stream_decode(&ds, &d->b_coder, d->z_d, d->z_idx,
                                     z_n);
    if (rc == RKVC_RANS_OK) {
        mlvc_px_extract_scales(d->z_d, d->s0, d->s1, d->YC, d->YH, d->YW,
                               d->ZH, d->ZW);
        rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y0_d, d->s0,
                                         y_n);
    }
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y1_d, d->s1,
                                         y_n);
    if (rc != RKVC_RANS_OK) {
        rkvc_rans_dec_stream_close(&ds);
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "rANS decode failed (corrupt stream?)");
        return (int)RKVC_STATUS_FORMAT;
    }
    rkvc_rans_dec_stream_close(&ds);

    /* NCHW int32 → NC1HWC2 fp16（与 NPU native 输入一致） */
    mlvc_px_nchw_to_nc1hwc2_fp16(d->z_d, d->z_d_nhwc, d->ZC, d->ZH, d->ZW);
    mlvc_px_nchw_to_nc1hwc2_fp16(d->y0_d, d->y0_d_nhwc, d->YC, d->YH, d->YW);
    mlvc_px_nchw_to_nc1hwc2_fp16(d->y1_d, d->y1_d_nhwc, d->YC, d->YH, d->YW);

    rknn_write_input(&d->dec_model, d->dec_z_in, d->z_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_y0_in, d->y0_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_y1_in, d->y1_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_ref_in, d->ref_nhwc);
    if (rknn_run(d->dec_model.ctx, NULL) != RKNN_SUCC) {
        push_reason(diag, RKVC_STATUS_HW, node, "decoder RKNN run failed");
        return (int)RKVC_STATUS_HW;
    }

    xh = rknn_output_data(&d->dec_model, d->dec_x_out);
    if (!xh || !fp16_tensor_is_finite(
                   xh, d->dec_model.native_out_attr[d->dec_x_out].n_elems)) {
        push_reason(diag, RKVC_STATUS_HW, node,
                    "decoder produced non-finite pixels");
        return (int)RKVC_STATUS_HW;
    }

    nv12_size = (size_t)d->IMG_W * d->IMG_H * 3 / 2;
    nv12 = malloc(nv12_size);
    if (!nv12)
        return (int)RKVC_STATUS_NOMEM;
    if (d->x_d2s_bs > 0) {
        /* NC1HWC2 → DepthToSpace 融合 → NCHW YUV fp16 → NV12 */
        int C1 = d->x_pre_c / d->OUT_C2;
        if (!d->x_img) {
            free(nv12);
            return (int)RKVC_STATUS_INTERNAL;
        }
        mlvc_px_nc1hwc2_d2s_dcr_f16(xh, d->x_img, C1, d->x_pre_h,
                                    d->x_pre_w, d->OUT_C2, d->x_d2s_bs);
        mlvc_px_nchw_yuv_fp16_to_nv12_planes(d->x_img, d->IMG_W, d->IMG_H,
                                             nv12, d->IMG_W,
                                             nv12 + (size_t)d->IMG_W *
                                                 d->IMG_H, d->IMG_W);
    } else {
        mlvc_px_nc1hwc2_fp16_to_nv12_planes(xh, d->IMG_W, d->IMG_H,
                                            d->OUT_C2, nv12, d->IMG_W,
                                            nv12 + (size_t)d->IMG_W *
                                                d->IMG_H, d->IMG_W);
    }

    /* decoder feature 输出 → 下一帧参考（native NC1HWC2 直接拷贝） */
    fv = rknn_output_data(&d->dec_model, d->dec_ref_out);
    if (!fv || !fp16_tensor_is_finite(
                   fv, d->dec_model.native_out_attr[d->dec_ref_out]
                           .n_elems)) {
        free(nv12);
        push_reason(diag, RKVC_STATUS_HW, node,
                    "decoder produced non-finite reference values");
        return (int)RKVC_STATUS_HW;
    }
    memcpy(d->ref_nhwc, fv,
           (size_t)d->dec_model.in_mem_attr[d->dec_ref_in].n_elems *
               sizeof(uint16_t));

    rkvc_frame_desc_init(&out_desc, sizeof(out_desc));
    out_desc.spec.width = (uint32_t)d->IMG_W;
    out_desc.spec.height = (uint32_t)d->IMG_H;
    out_desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    out_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    out_desc.spec.stride = (uint32_t)d->IMG_W;
    out_desc.spec.ver_stride = (uint32_t)d->IMG_H;
    out_desc.data = nv12;
    out_desc.size = nv12_size;
    if (keyframe || d->frame_count == 0)
        out_desc.flags |= RKVC_FRAME_FLAG_KEYFRAME;
    st = rkvc_backend_frame_create(&out_desc, free_buffer_fn, nv12, &output);
    if (st != RKVC_STATUS_OK) {
        free(nv12);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    d->frame_count++;
    return rc;
}

static int mlvc_dec_process(rkvc_node *node, rkvc_frame *input,
                            rkvc_diag **diag)
{
    struct mlvc_decoder *d = node->priv;
    rkvc_frame_desc desc;
    int rc;

    if (!input || rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        desc.spec.fmt != RKVC_FRAME_FMT_BITSTREAM ||
        desc.spec.domain != RKVC_MEM_DOMAIN_HOST || !desc.data || !desc.size)
        return (int)RKVC_STATUS_FORMAT;

    rc = mlvc_demux_append(&d->demux, desc.data, desc.size);
    if (rc != 0)
        return rc;

    for (;;) {
        const uint8_t *payload;
        size_t size;
        int keyframe;

        rc = mlvc_demux_next(&d->demux, &payload, &size, &keyframe);
        if (rc < 0) {
            push_reason(diag, RKVC_STATUS_FORMAT, node,
                        "malformed .mlvc stream");
            return (int)RKVC_STATUS_FORMAT;
        }
        /* 头解析成功后（qp 已知）惰性初始化 RKNN；demux 缓冲未消费，
         * 重跑循环以同一帧载荷进入已就绪的解码路径。 */
        if (!d->model_ready) {
            if (!d->demux.have_header)
                return 0; /* 缓冲不足：等下一块 */
            d->qp = (int)d->demux.hdr.qp;
            rc = dec_lazy_init(d, diag, node);
            if (rc != 0)
                return rc;
            continue;
        }
        if (rc == 0)
            return 0; /* 缓冲不足：等下一块 */
        rc = dec_decode_frame(d, node, payload, size, keyframe, diag);
        if (rc != 0)
            return rc;
        mlvc_demux_consume(&d->demux, MLVC_REC_SIZE + size);
    }
}

static void mlvc_dec_close(rkvc_node *node)
{
    struct mlvc_decoder *d = node ? node->priv : NULL;
    if (!d)
        return;
    dec_free_bufs(d);
    rknn_model_cleanup(&d->dec_model);
    mlvc_demux_free(&d->demux);
    if (d->rans_ready) {
        rkvc_rans_coder_free(&d->g_coder);
        rkvc_rans_coder_free(&d->b_coder);
        d->rans_ready = 0;
    }
}

static const rkvc_node_ops mlvc_dec_ops = {
    "mlvc.decode", mlvc_dec_configure, mlvc_dec_open, mlvc_dec_process,
    NULL, mlvc_dec_close, mlvc_destroy_node, mlvc_dec_bind_model,
};

/* ════════════════════════════════════════════════════════════════════ */
/*  工厂与后端描述符                                                     */
/* ════════════════════════════════════════════════════════════════════ */

static int mlvc_enc_matches(rkvc_operation op, rkvc_codec codec,
                            const rkvc_device_caps *caps)
{
    (void)caps;
    return op == RKVC_OPERATION_ENCODE && codec == RKVC_CODEC_MLVC;
}

static int mlvc_dec_matches(rkvc_operation op, rkvc_codec codec,
                            const rkvc_device_caps *caps)
{
    (void)caps;
    return op == RKVC_OPERATION_DECODE && codec == RKVC_CODEC_MLVC;
}

static rkvc_node *mlvc_node_create(const rkvc_node_factory *factory,
                                   const rkvc_request *request,
                                   void *create_ctx)
{
    int is_encode = factory->stage == RKVC_NODE_STAGE_ENCODE;
    rkvc_node *node = calloc(1, sizeof(*node));
    void *priv = calloc(1, is_encode ? sizeof(struct mlvc_encoder)
                                     : sizeof(struct mlvc_decoder));
    (void)create_ctx;
    if (!node || !priv) {
        free(node);
        free(priv);
        return NULL;
    }
    node->ops = is_encode ? &mlvc_enc_ops : &mlvc_dec_ops;
    node->priv = priv;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        mlvc_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = is_encode ? "video" : "bitstream";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = is_encode ? "bitstream" : "video";
    if (is_encode)
        ((struct mlvc_encoder *)priv)->request = *request;
    else
        ((struct mlvc_decoder *)priv)->request = *request;
    return node;
}

static int mlvc_probe(const rkvc_device_caps *caps, void *probe_ctx,
                      rkvc_diag **diag)
{
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    return 0; /* RKNN 可用性由模型绑定与 open 阶段裁决 */
}

static const rkvc_node_factory mlvc_factories[] = {
    {
        .id = "mlvc.encode",
        .backend_id = "mlvc",
        .stage = RKVC_NODE_STAGE_ENCODE,
        .priority = 1100,
        .matches = mlvc_enc_matches,
        .create = mlvc_node_create,
    },
    {
        .id = "mlvc.decode",
        .backend_id = "mlvc",
        .stage = RKVC_NODE_STAGE_DECODE,
        .priority = 1100,
        .matches = mlvc_dec_matches,
        .create = mlvc_node_create,
    },
};

static const rkvc_node_factory *mlvc_factory_list(void *probe_ctx,
                                                  size_t *count)
{
    (void)probe_ctx;
    *count = sizeof(mlvc_factories) / sizeof(mlvc_factories[0]);
    return mlvc_factories;
}

static const rkvc_backend mlvc_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "mlvc",
    .capability_flags = RKVC_BACKEND_CAP_RKNN,
    .probe = mlvc_probe,
    .factories = mlvc_factory_list,
};

const rkvc_backend *rkvc_backend_query(void) { return &mlvc_backend; }
