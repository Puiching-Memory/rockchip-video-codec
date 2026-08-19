/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * MLVC 神经视频编解码节点（RKNN NPU + rANS 熵编码）— 纯 C 实现。
 *
 * 完整移植自 node_mlvc.cpp（C++），消除 msrtc_rans / C++17 依赖：
 *   - 熵编解码改用 lib/rans.c（本仓纯 C rANS 实现）
 *   - RKNN 零拷贝 I/O（native NC1HWC2 fp16）
 *   - 编码器：YUV(NV12) → encoder NPU → rANS 熵编码 → 码流包
 *     （feature 输出直接作为下一帧参考，无需运行 decoder NPU）
 *   - 解码器：码流包 → rANS 熵解码 → decoder NPU → YUV(NV12)
 *   - 自定义 .mlvc 容器（FFmpeg 无法封装原始 rANS 码流）
 */

#ifdef RKVC_ENABLE_MLVC

#include "internal.h"
#include "platform.h"
#include "rans.h"
#include "qppatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <rknn_api.h>

#include "rknn_util.h"
#include "mlvc_pixel.h"

/* ── fp16 辅助 ──────────────────────────────────────────────────────
 * fp16 转换与每帧像素/张量转换内核见 lib/mlvc_pixel.{h,c}
 * （NEON 快速路径 + 标量回退，逐位等价）。 */
/* ── PMF 二进制表加载 ────────────────────────────────────────────── */

typedef struct {
    int32_t *lengths;
    int32_t *offsets;
    int32_t *table;
    uint32_t num_lengths;
    uint32_t num_offsets;
    uint32_t num_table;
    double scale_min;
    double scale_max;
    uint32_t scale_levels;
    uint32_t index_space;
    uint32_t qp_num;
    uint32_t channels;
} mlvc_pmf;

/* lengths/offsets 上界 1M 项；table 上界 16M 项（约 64MB），挡住计数回绕后的 fread 堆溢出。 */
#define MLVC_PMF_MAX_LEN  (1u << 20)
#define MLVC_PMF_MAX_TAB  (16u << 20)

static void free_pmf(mlvc_pmf *p)
{
    if (!p)
        return;
    rkvc_free(p->lengths);
    rkvc_free(p->offsets);
    rkvc_free(p->table);
    memset(p, 0, sizeof(*p));
}

static rkvc_err load_pmf(mlvc_pmf *p, const char *path)
{
    FILE *f = NULL;
    rkvc_err err = RKVC_ERR_FORMAT;

    memset(p, 0, sizeof(*p));
    f = fopen(path, "rb");
    if (!f)
        return RKVC_ERR_IO;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PMF1", 4) != 0)
        goto fail;

    uint32_t nL, nO, nT;
    if (fread(&nL, 4, 1, f) != 1 || fread(&nO, 4, 1, f) != 1 ||
        fread(&nT, 4, 1, f) != 1)
        goto fail;
    if (nL == 0 || nL > MLVC_PMF_MAX_LEN ||
        nO == 0 || nO > MLVC_PMF_MAX_LEN ||
        nT == 0 || nT > MLVC_PMF_MAX_TAB)
        goto fail;

    p->num_lengths = nL;
    p->num_offsets = nO;
    p->num_table = nT;
    p->lengths = rkvc_malloc((size_t)nL * 4u);
    p->offsets = rkvc_malloc((size_t)nO * 4u);
    p->table   = rkvc_malloc((size_t)nT * 4u);
    if (!p->lengths || !p->offsets || !p->table) {
        err = RKVC_ERR_NOMEM;
        goto fail;
    }
    if (fread(p->lengths, 4, nL, f) != nL ||
        fread(p->offsets, 4, nO, f) != nO ||
        fread(p->table, 4, nT, f) != nT)
        goto fail;

    uint32_t tag = 0;
    if (fread(&tag, 4, 1, f) != 1)
        goto fail;
    if (tag == 1) {
        if (fread(&p->scale_min, 8, 1, f) != 1 ||
            fread(&p->scale_max, 8, 1, f) != 1 ||
            fread(&p->scale_levels, 4, 1, f) != 1 ||
            fread(&p->index_space, 4, 1, f) != 1)
            goto fail;
    } else if (tag == 2) {
        if (fread(&p->qp_num, 4, 1, f) != 1 ||
            fread(&p->channels, 4, 1, f) != 1)
            goto fail;
    } else {
        goto fail;
    }
    fclose(f);
    return RKVC_OK;

fail:
    fclose(f);
    free_pmf(p);
    return err;
}

/* ── RKNN 零拷贝模型封装 ─────────────────────────────────────────── */

#define MLVC_RKNN_CHECK(call)                                                   \
    do {                                                                        \
        int rc_ = (call);                                                       \
        if (rc_ != RKNN_SUCC) {                                                 \
            RKVC_LOG("RKNN error %d at %s:%d", rc_, __FILE__, __LINE__);        \
            return RKVC_ERR_HW;                                                 \
        }                                                                       \
    } while (0)

#define MLVC_MAX_IO 8
#define MLVC_MAX_MODEL_BYTES ((size_t)256 * 1024 * 1024)

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr in_attr[MLVC_MAX_IO];
    rknn_tensor_attr out_attr[MLVC_MAX_IO];
    rknn_tensor_attr native_out_attr[MLVC_MAX_IO];
    rknn_tensor_mem *in_mem[MLVC_MAX_IO];
    rknn_tensor_mem *out_mem[MLVC_MAX_IO];
    char *model_buf;
    size_t model_size;
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
        if (m->in_mem[i]) rknn_destroy_mem(m->ctx, m->in_mem[i]);
    for (uint32_t i = 0; i < no; i++)
        if (m->out_mem[i]) rknn_destroy_mem(m->ctx, m->out_mem[i]);
    if (m->ctx) rknn_destroy(m->ctx);
    rkvc_free(m->model_buf);
    memset(m, 0, sizeof(*m));
}

static rkvc_err rknn_model_init(mlvc_rknn_model *m, const char *path,
                                const char *patch_path, int expected_qp)
{
    memset(m, 0, sizeof(*m));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return RKVC_ERR_IO;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz <= 0 || (size_t)sz > MLVC_MAX_MODEL_BYTES) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    m->model_buf = rkvc_malloc((size_t)sz);
    if (!m->model_buf) {
        fclose(fp);
        return RKVC_ERR_NOMEM;
    }
    if (fread(m->model_buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        rknn_model_cleanup(m);
        return RKVC_ERR_IO;
    }
    fclose(fp);
    m->model_size = (size_t)sz;

    if (patch_path && patch_path[0]) {
        rkvc_err perr = rkvc_qppatch_apply_file((uint8_t *)m->model_buf,
                                                m->model_size, patch_path,
                                                expected_qp);
        if (perr) {
            RKVC_LOG("MLVC qp patch apply failed: %s", patch_path);
            rknn_model_cleanup(m);
            return perr;
        }
        RKVC_LOG("MLVC applied qp patch %s", patch_path);
    }

    /* 分发构建：在 rknn_init 前强制 librknnrt 静默，避免 RKNN_LOG_LEVEL
     * 泄露已解密模型的网络结构（见 rkvc_rknn_quiet_runtime 说明）。 */
    rkvc_rknn_quiet_runtime();
    int rc = rknn_init(&m->ctx, m->model_buf, (uint32_t)sz, 0, NULL);
    if (rc != RKNN_SUCC) {
        RKVC_LOG("RKNN error %d at %s:%d", rc, __FILE__, __LINE__);
        rknn_model_cleanup(m);
        return RKVC_ERR_HW;
    }
    /*
     * 多核 NPU（如 RK3588 三核）显式启用全部核心：默认 AUTO 仅单核调度，
     * 显式 core_mask 让运行时在核间分配算子，显著降低编码推理延迟
     *（RK3588 受控实测编码 +24%）。核心数来自平台探测，并由驱动自适应
     * 校正（rkvc_rknn_apply_npu_cores 向下尝试）；单核平台（如 RV1126B）
     * 保持默认单核行为，不调用 set_core_mask。
     */
    {
        const rkvc_platform_info *pi = rkvc_platform_probe();
        if (pi->has_npu && pi->npu_cores > 1)
            rkvc_rknn_apply_npu_cores(m->ctx, pi->npu_cores);
    }
    rc = rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io_num, sizeof(m->io_num));
    if (rc != RKNN_SUCC) {
        RKVC_LOG("RKNN error %d at %s:%d", rc, __FILE__, __LINE__);
        rknn_model_cleanup(m);
        return RKVC_ERR_HW;
    }
    if (m->io_num.n_input > MLVC_MAX_IO || m->io_num.n_output > MLVC_MAX_IO) {
        RKVC_LOG("RKNN I/O count %u/%u exceeds %d",
                 m->io_num.n_input, m->io_num.n_output, MLVC_MAX_IO);
        rknn_model_cleanup(m);
        return RKVC_ERR_FORMAT;
    }

    for (uint32_t i = 0; i < m->io_num.n_input; i++) {
        m->in_attr[i].index = i;
        rc = rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &m->in_attr[i],
                        sizeof(m->in_attr[i]));
        if (rc != RKNN_SUCC) {
            RKVC_LOG("RKNN error %d at %s:%d", rc, __FILE__, __LINE__);
            rknn_model_cleanup(m);
            return RKVC_ERR_HW;
        }
    }
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        m->out_attr[i].index = i;
        rc = rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &m->out_attr[i],
                        sizeof(m->out_attr[i]));
        if (rc != RKNN_SUCC) {
            RKVC_LOG("RKNN error %d at %s:%d", rc, __FILE__, __LINE__);
            rknn_model_cleanup(m);
            return RKVC_ERR_HW;
        }
    }
    return RKVC_OK;
}

static int rknn_find_input(const mlvc_rknn_model *m, const char *key)
{
    for (uint32_t i = 0; i < m->io_num.n_input; i++)
        if (strstr(m->in_attr[i].name, key)) return (int)i;
    return -1;
}

static int rknn_find_output(const mlvc_rknn_model *m, const char *key)
{
    for (uint32_t i = 0; i < m->io_num.n_output; i++)
        if (strstr(m->out_attr[i].name, key)) return (int)i;
    return -1;
}

static rkvc_err rknn_alloc_input_mems(mlvc_rknn_model *m)
{
    for (uint32_t i = 0; i < m->io_num.n_input; i++) {
        rknn_tensor_attr a = m->in_attr[i];
        a.index = i;
        a.type = RKNN_TENSOR_FLOAT16;
        a.fmt = RKNN_TENSOR_NHWC;
        a.pass_through = 1;
        a.size = a.size_with_stride;
        m->in_mem[i] = rknn_create_mem(m->ctx, a.size_with_stride);
        if (!m->in_mem[i]) return RKVC_ERR_HW;
        MLVC_RKNN_CHECK(rknn_set_io_mem(m->ctx, m->in_mem[i], &a));
    }
    return RKVC_OK;
}

static rkvc_err rknn_alloc_output_mems(mlvc_rknn_model *m)
{
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        MLVC_RKNN_CHECK(rknn_query(m->ctx, RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR, &a, sizeof(a)));
        a.index = i;
        a.pass_through = 1;
        m->native_out_attr[i] = a;
        m->out_mem[i] = rknn_create_mem(m->ctx, a.size_with_stride);
        if (!m->out_mem[i]) return RKVC_ERR_HW;
        MLVC_RKNN_CHECK(rknn_set_io_mem(m->ctx, m->out_mem[i], &a));
    }
    return RKVC_OK;
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
    if (!m || i < 0 || (uint32_t)i >= m->io_num.n_input || !m->in_mem[i] || !data)
        return;
    rknn_tensor_attr *a = &m->in_attr[i];
    if (a->w_stride == 0 || a->w_stride == (uint32_t)a->dims[2]) {
        memcpy(m->in_mem[i]->virt_addr, data, (size_t)a->n_elems * sizeof(uint16_t));
    } else {
        int H = a->dims[1], W = a->dims[2], C = a->dims[3];
        uint16_t *dst = (uint16_t *)m->in_mem[i]->virt_addr;
        for (int h = 0; h < H; h++)
            memcpy(dst + (size_t)h * a->w_stride * C,
                   data + (size_t)h * W * C,
                   (size_t)W * C * sizeof(uint16_t));
    }
    rknn_mem_sync(m->ctx, m->in_mem[i], RKNN_MEMORY_SYNC_TO_DEVICE);
}

/* ── NC1HWC2 → NV12 / YUV → NHWC 包装（纯数组内核在 mlvc_pixel.c）── */

static rkvc_err nchw_yuv_fp16_to_nv12(const uint16_t *src, int W, int H,
                                      rkvc_buffer **out)
{
    rkvc_buffer *b = NULL;
    rkvc_err err = rkvc_buffer_alloc_video_host(&b, W, H, RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    AVFrame *avf = b->av_frame;
    /* 饱和内核融合了原 clip(0,1) 遍：越界值由整数饱和直接覆盖 */
    mlvc_px_nchw_yuv_fp16_to_nv12_planes(src, W, H,
                                         avf->data[0], avf->linesize[0],
                                         avf->data[1], avf->linesize[1]);
    *out = b;
    return RKVC_OK;
}

/* ── YUV → fp16 NHWC ── */

static void yuv_to_nhwc_fp16(const rkvc_buffer *frame, uint16_t *nhwc)
{
    AVFrame *avf = frame->av_frame;
    int nv12 = (frame->format == RKVC_PIX_FMT_NV12);
    const uint8_t *Up = avf->data[1];
    const uint8_t *Vp = nv12 ? Up + 1 : avf->data[2];
    mlvc_px_yuv_to_nhwc_fp16(avf->data[0], avf->linesize[0], Up, Vp,
                             avf->linesize[1], nv12,
                             (int)frame->width, (int)frame->height, nhwc);
}

/* ── fp16 NHWC → NV12 ── */

static rkvc_err nc1hwc2_fp16_to_nv12(const uint16_t *src, int W, int H, int c2,
                                     rkvc_buffer **out)
{
    rkvc_buffer *b = NULL;
    rkvc_err err = rkvc_buffer_alloc_video_host(&b, W, H, RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    AVFrame *avf = b->av_frame;
    mlvc_px_nc1hwc2_fp16_to_nv12_planes(src, W, H, c2,
                                        avf->data[0], avf->linesize[0],
                                        avf->data[1], avf->linesize[1]);
    *out = b;
    return RKVC_OK;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  MLVC 编码器                                                         */
/* ════════════════════════════════════════════════════════════════════ */

struct rkvc_mlvc_enc {
    mlvc_rknn_model enc_model;
    rkvc_rans_coder g_coder, b_coder;
    int qp;

    int IMG_W, IMG_H, REF_C, REF_H, REF_W;
    int ZC, ZH, ZW, YC, YH, YW;
    int enc_x_in, enc_ref_in;
    int enc_feat_out, enc_z_out, enc_y0_out, enc_y1_out;

    /* 工作缓冲 */
    uint16_t *x_nhwc;
    uint16_t *ref_nhwc;
    int32_t *z_r, *y0_r, *y1_r, *s0, *s1, *z_idx;

    rkvc_buffer *pending_pkt;
    int frame_count;
    int flushed;
};

static rkvc_err enc_resolve_geom(rkvc_mlvc_enc *e)
{
    mlvc_rknn_model *enc = &e->enc_model;
    e->enc_x_in = rknn_find_input(enc, "x");
    if (e->enc_x_in < 0)
        e->enc_x_in = rknn_find_input(enc, "New_input_x");
    e->enc_ref_in = rknn_find_input(enc, "ref_feature");
    if (e->enc_x_in < 0 || e->enc_ref_in < 0)
        return RKVC_ERR_FORMAT;

    e->IMG_H = enc->in_attr[e->enc_x_in].dims[1];
    e->IMG_W = enc->in_attr[e->enc_x_in].dims[2];
    e->REF_H = enc->in_attr[e->enc_ref_in].dims[1];
    e->REF_W = enc->in_attr[e->enc_ref_in].dims[2];
    e->REF_C = enc->in_attr[e->enc_ref_in].dims[3];

    e->enc_feat_out = rknn_find_output(enc, "feature");
    e->enc_z_out    = rknn_find_output(enc, "z_raw");
    e->enc_y0_out   = rknn_find_output(enc, "y_raw_0");
    e->enc_y1_out   = rknn_find_output(enc, "y_raw_1");
    if (e->enc_feat_out < 0 || e->enc_z_out < 0 ||
        e->enc_y0_out < 0 || e->enc_y1_out < 0)
        return RKVC_ERR_FORMAT;

    int zC1 = enc->native_out_attr[e->enc_z_out].dims[1];
    e->ZC = zC1 * 8;
    e->ZH = enc->native_out_attr[e->enc_z_out].dims[2];
    e->ZW = enc->native_out_attr[e->enc_z_out].dims[3];

    int yC1 = enc->native_out_attr[e->enc_y0_out].dims[1];
    e->YC = yC1 * 8;
    e->YH = enc->native_out_attr[e->enc_y0_out].dims[2];
    e->YW = enc->native_out_attr[e->enc_y0_out].dims[3];

    return RKVC_OK;
}

static rkvc_err enc_alloc_bufs(rkvc_mlvc_enc *e)
{
    size_t img_sz   = (size_t)e->IMG_H * e->IMG_W;
    size_t ref_sz   = (size_t)e->REF_C * e->REF_H * e->REF_W;
    size_t z_sz     = (size_t)e->ZC * e->ZH * e->ZW;
    size_t y_sz     = (size_t)e->YC * e->YH * e->YW;

    e->x_nhwc  = rkvc_malloc(img_sz * 3 * 2);
    e->ref_nhwc = rkvc_malloc(ref_sz * 2);
    e->z_r     = rkvc_malloc(z_sz * 4);
    e->y0_r    = rkvc_malloc(y_sz * 4);
    e->y1_r    = rkvc_malloc(y_sz * 4);
    e->s0      = rkvc_malloc(y_sz * 4);
    e->s1      = rkvc_malloc(y_sz * 4);
    e->z_idx   = rkvc_malloc(z_sz * 4);
    if (!e->x_nhwc || !e->ref_nhwc || !e->z_r || !e->y0_r || !e->y1_r ||
        !e->s0 || !e->s1 || !e->z_idx)
        return RKVC_ERR_NOMEM;

    memset(e->ref_nhwc, 0, ref_sz * 2);
    /* z_idx 在通道平面内为常量：按平面填充（原逐元素三重循环）*/
    for (int c = 0; c < e->ZC; c++) {
        int32_t *p = e->z_idx + (size_t)c * e->ZH * e->ZW;
        size_t n = (size_t)e->ZH * e->ZW;
        for (size_t i = 0; i < n; i++)
            p[i] = e->qp * e->ZC + c;
    }
    return RKVC_OK;
}

static void enc_free_bufs(rkvc_mlvc_enc *e)
{
    rkvc_free(e->x_nhwc);
    rkvc_free(e->ref_nhwc);
    rkvc_free(e->z_r);
    rkvc_free(e->y0_r);
    rkvc_free(e->y1_r);
    rkvc_free(e->s0);
    rkvc_free(e->s1);
    rkvc_free(e->z_idx);
}

rkvc_err rkvc_mlvc_enc_open(rkvc_mlvc_enc **out, const rkvc_mlvc_enc_config *cfg)
{
    if (!out || !cfg || !cfg->enc_model_path ||
        !cfg->gaussian_pmf_path || !cfg->bitest_pmf_path)
        return RKVC_ERR_INVALID;
    *out = NULL;

    rkvc_mlvc_enc *e = rkvc_calloc(1, sizeof(*e));
    if (!e) return RKVC_ERR_NOMEM;
    e->qp = cfg->qp > 0 ? cfg->qp : 21;

    /* PMF 表 */
    mlvc_pmf gpmf = {0}, bpmf = {0};
    rkvc_err err = load_pmf(&gpmf, cfg->gaussian_pmf_path);
    if (err) { rkvc_free(e); return err; }
    err = load_pmf(&bpmf, cfg->bitest_pmf_path);
    if (err) { free_pmf(&gpmf); rkvc_free(e); return err; }

    if (!gpmf.index_space) {
        RKVC_LOG("gaussian.bin: expect index_space=True");
        free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(e);
        return RKVC_ERR_FORMAT;
    }

    /* 熵编解码器 */
    int rc = rkvc_rans_coder_init(&e->g_coder, RKVC_RANS_BYTE,
                                  gpmf.lengths, gpmf.num_lengths,
                                  gpmf.offsets, gpmf.num_offsets,
                                  gpmf.table, gpmf.num_table, 16, 2);
    if (rc) { free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(e); return RKVC_ERR_INTERNAL; }
    rc = rkvc_rans_coder_init(&e->b_coder, RKVC_RANS_BYTE,
                              bpmf.lengths, bpmf.num_lengths,
                              bpmf.offsets, bpmf.num_offsets,
                              bpmf.table, bpmf.num_table, 16, 2);
    if (rc) { rkvc_rans_coder_free(&e->g_coder); free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(e); return RKVC_ERR_INTERNAL; }

    free_pmf(&gpmf);
    free_pmf(&bpmf);

    /* 模型 */
    err = rknn_model_init(&e->enc_model, cfg->enc_model_path,
                          cfg->qp_patch_path, e->qp);
    if (err) { rkvc_rans_coder_free(&e->g_coder); rkvc_rans_coder_free(&e->b_coder); rkvc_free(e); return err; }

    err = rknn_alloc_input_mems(&e->enc_model);
    if (err) goto fail_model;
    err = rknn_alloc_output_mems(&e->enc_model);
    if (err) goto fail_model;

    err = enc_resolve_geom(e);
    if (err) goto fail_model;
    err = enc_alloc_bufs(e);
    if (err) goto fail_model;

    *out = e;
    return RKVC_OK;

fail_model:
    enc_free_bufs(e);
    rknn_model_cleanup(&e->enc_model);
    rkvc_rans_coder_free(&e->g_coder);
    rkvc_rans_coder_free(&e->b_coder);
    rkvc_free(e);
    return err;
}

void rkvc_mlvc_enc_close(rkvc_mlvc_enc *e)
{
    if (!e) return;
    rkvc_buffer_unref(e->pending_pkt);
    enc_free_bufs(e);
    rknn_model_cleanup(&e->enc_model);
    rkvc_rans_coder_free(&e->g_coder);
    rkvc_rans_coder_free(&e->b_coder);
    rkvc_free(e);
}

int rkvc_mlvc_enc_width(const rkvc_mlvc_enc *e)  { return e ? e->IMG_W : 0; }
int rkvc_mlvc_enc_height(const rkvc_mlvc_enc *e) { return e ? e->IMG_H : 0; }

rkvc_err rkvc_mlvc_enc_send_frame(rkvc_mlvc_enc *e, rkvc_buffer *frame)
{
    if (!e) return RKVC_ERR_INVALID;
    if (e->flushed) return RKVC_ERR_EOF;
    if (!frame) return rkvc_mlvc_enc_drain(e);
    if (e->pending_pkt) return RKVC_ERR_AGAIN;

    if (frame->width != (uint32_t)e->IMG_W || frame->height != (uint32_t)e->IMG_H) {
        RKVC_LOG("frame size mismatch: model expects %dx%d", e->IMG_W, e->IMG_H);
        return RKVC_ERR_FORMAT;
    }

    yuv_to_nhwc_fp16(frame, e->x_nhwc);

    /* encoder NPU */
    rknn_write_input(&e->enc_model, e->enc_x_in, e->x_nhwc);
    rknn_write_input(&e->enc_model, e->enc_ref_in, e->ref_nhwc);
    MLVC_RKNN_CHECK(rknn_run(e->enc_model.ctx, NULL));

    const uint16_t *bz  = rknn_output_data(&e->enc_model, e->enc_z_out);
    const uint16_t *by0 = rknn_output_data(&e->enc_model, e->enc_y0_out);
    const uint16_t *by1 = rknn_output_data(&e->enc_model, e->enc_y1_out);
    if (!bz || !by0 || !by1)
        return RKVC_ERR_HW;

    mlvc_px_nc1hwc2_to_nchw(bz, e->z_r, e->ZC, e->ZH, e->ZW);
    mlvc_px_nc1hwc2_to_nchw(by0, e->y0_r, e->YC, e->YH, e->YW);
    mlvc_px_nc1hwc2_to_nchw(by1, e->y1_r, e->YC, e->YH, e->YW);

    /* 熵编码: push y1, y0, z */
    mlvc_px_extract_scales(e->z_r, e->s0, e->s1, e->YC, e->YH, e->YW, e->ZH, e->ZW);

    rkvc_rans_enc_stream stream;
    rkvc_rans_enc_stream_init(&stream, RKVC_RANS_BYTE, 65536);
    int rc = rkvc_rans_enc_stream_encode(&stream, &e->g_coder, e->s1, e->y1_r,
                                         (size_t)e->YC * e->YH * e->YW);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_enc_stream_encode(&stream, &e->g_coder, e->s0, e->y0_r,
                                         (size_t)e->YC * e->YH * e->YW);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_enc_stream_encode(&stream, &e->b_coder, e->z_idx, e->z_r,
                                         (size_t)e->ZC * e->ZH * e->ZW);
    if (rc != RKVC_RANS_OK) {
        rkvc_rans_enc_stream_free(&stream);
        return RKVC_ERR_INTERNAL;
    }

    size_t bits_size;
    const uint8_t *bits = rkvc_rans_enc_stream_flush(&stream, &bits_size);
    if (!bits) {
        rkvc_rans_enc_stream_free(&stream);
        return RKVC_ERR_INTERNAL;
    }

    rkvc_buffer *pkt = NULL;
    rkvc_err err = rkvc_buffer_alloc_bitstream(&pkt, bits, bits_size, 1);
    rkvc_rans_enc_stream_free(&stream);
    if (err != RKVC_OK) return err;

    pkt->pts = frame->pts;
    pkt->dts = frame->pts;
    pkt->key_frame = (e->frame_count == 0);
    e->pending_pkt = pkt;

    /* encoder feature 输出 → 下一帧参考（native NC1HWC2 直接拷贝）*/
    const uint16_t *fv = rknn_output_data(&e->enc_model, e->enc_feat_out);
    if (!fv)
        return RKVC_ERR_HW;
    memcpy(e->ref_nhwc, fv, (size_t)e->REF_C * e->REF_H * e->REF_W * sizeof(uint16_t));

    e->frame_count++;
    return RKVC_OK;
}

rkvc_err rkvc_mlvc_enc_receive_packet(rkvc_mlvc_enc *e, rkvc_buffer **pkt)
{
    if (!e || !pkt) return RKVC_ERR_INVALID;
    *pkt = NULL;
    if (e->pending_pkt) {
        *pkt = e->pending_pkt;
        e->pending_pkt = NULL;
        return RKVC_OK;
    }
    return e->flushed ? RKVC_ERR_EOF : RKVC_ERR_AGAIN;
}

rkvc_err rkvc_mlvc_enc_drain(rkvc_mlvc_enc *e)
{
    if (!e) return RKVC_ERR_INVALID;
    e->flushed = 1;
    return RKVC_OK;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  MLVC 解码器                                                         */
/* ════════════════════════════════════════════════════════════════════ */

struct rkvc_mlvc_dec {
    mlvc_rknn_model dec_model;
    rkvc_rans_coder g_coder, b_coder;
    int qp;

    int IMG_W, IMG_H, REF_C, REF_H, REF_W;
    int OUT_C2;  /* decoder x_hat 输出 NC1HWC2 的 C2 维（通常 8）*/
    int x_d2s_bs; /* >0：x_hat 是 shuffle 前的 head conv（C=3*bs*bs） */
    int x_pre_c, x_pre_h, x_pre_w;
    int ZC, ZH, ZW, YC, YH, YW;
    int dec_z_in, dec_y0_in, dec_y1_in, dec_ref_in, dec_x_out, dec_ref_out;

    uint16_t *ref_nhwc;
    int32_t *z_d, *y0_d, *y1_d, *s0, *s1, *z_idx;
    uint16_t *z_d_nhwc, *y0_d_nhwc, *y1_d_nhwc;
    uint16_t *x_img;

    rkvc_buffer *pending_pkt;
    rkvc_buffer *out_frame;
    int frame_count;
    int input_eof;
};

static rkvc_err dec_resolve_geom(rkvc_mlvc_dec *d)
{
    mlvc_rknn_model *dec = &d->dec_model;
    d->dec_z_in    = rknn_find_input(dec, "z_raw");
    d->dec_y0_in   = rknn_find_input(dec, "y_raw_0");
    d->dec_y1_in   = rknn_find_input(dec, "y_raw_1");
    d->dec_ref_in  = rknn_find_input(dec, "ref_feature");
    d->dec_x_out   = rknn_find_output(dec, "x_hat");
    d->dec_ref_out = rknn_find_output(dec, "feature");
    if (d->dec_z_in < 0 || d->dec_y0_in < 0 || d->dec_y1_in < 0 ||
        d->dec_ref_in < 0 || d->dec_x_out < 0 || d->dec_ref_out < 0)
        return RKVC_ERR_FORMAT;

    int C1 = (int)dec->native_out_attr[d->dec_x_out].dims[1];
    int native_h = (int)dec->native_out_attr[d->dec_x_out].dims[2];
    int native_w = (int)dec->native_out_attr[d->dec_x_out].dims[3];
    d->OUT_C2 = (dec->native_out_attr[d->dec_x_out].n_dims >= 5)
              ? (int)dec->native_out_attr[d->dec_x_out].dims[4] : 8;
    if (d->OUT_C2 < 3) d->OUT_C2 = 3;
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
                RKVC_LOG("MLVC decoder x_hat is pre-shuffle %dx%dx%d, CPU DepthToSpace DCR bs=%d → %dx%d",
                         d->x_pre_c, native_h, native_w, s, d->IMG_W, d->IMG_H);
                break;
            }
        }
    }
    d->REF_C = dec->in_attr[d->dec_ref_in].dims[3];
    d->REF_H = dec->in_attr[d->dec_ref_in].dims[1];
    d->REF_W = dec->in_attr[d->dec_ref_in].dims[2];

    d->ZC = dec->in_attr[d->dec_z_in].dims[3];
    d->ZH = dec->in_attr[d->dec_z_in].dims[1];
    d->ZW = dec->in_attr[d->dec_z_in].dims[2];
    d->YC = dec->in_attr[d->dec_y0_in].dims[3];
    d->YH = dec->in_attr[d->dec_y0_in].dims[1];
    d->YW = dec->in_attr[d->dec_y0_in].dims[2];
    return RKVC_OK;
}

static rkvc_err dec_alloc_bufs(rkvc_mlvc_dec *d)
{
    size_t ref_sz = (size_t)d->REF_C * d->REF_H * d->REF_W;
    size_t z_sz   = (size_t)d->ZC * d->ZH * d->ZW;
    size_t y_sz   = (size_t)d->YC * d->YH * d->YW;

    d->ref_nhwc  = rkvc_malloc(ref_sz * 2);
    d->z_d       = rkvc_malloc(z_sz * 4);
    d->y0_d      = rkvc_malloc(y_sz * 4);
    d->y1_d      = rkvc_malloc(y_sz * 4);
    d->s0        = rkvc_malloc(y_sz * 4);
    d->s1        = rkvc_malloc(y_sz * 4);
    d->z_idx     = rkvc_malloc(z_sz * 4);
    d->z_d_nhwc  = rkvc_malloc(z_sz * 2);
    d->y0_d_nhwc = rkvc_malloc(y_sz * 2);
    d->y1_d_nhwc = rkvc_malloc(y_sz * 2);
    d->x_img = NULL;
    if (d->x_d2s_bs > 0)
        d->x_img = rkvc_malloc(3ull * d->IMG_H * d->IMG_W * 2);
    if (!d->ref_nhwc || !d->z_d || !d->y0_d || !d->y1_d || !d->s0 || !d->s1 ||
        !d->z_idx || !d->z_d_nhwc || !d->y0_d_nhwc || !d->y1_d_nhwc ||
        (d->x_d2s_bs > 0 && !d->x_img))
        return RKVC_ERR_NOMEM;

    memset(d->ref_nhwc, 0, ref_sz * 2);
    /* z_idx 在通道平面内为常量：按平面填充（原逐元素三重循环）*/
    for (int c = 0; c < d->ZC; c++) {
        int32_t *p = d->z_idx + (size_t)c * d->ZH * d->ZW;
        size_t n = (size_t)d->ZH * d->ZW;
        for (size_t i = 0; i < n; i++)
            p[i] = d->qp * d->ZC + c;
    }
    return RKVC_OK;
}

static void dec_free_bufs(rkvc_mlvc_dec *d)
{
    rkvc_free(d->ref_nhwc);
    rkvc_free(d->z_d);
    rkvc_free(d->y0_d);
    rkvc_free(d->y1_d);
    rkvc_free(d->s0);
    rkvc_free(d->s1);
    rkvc_free(d->z_idx);
    rkvc_free(d->z_d_nhwc);
    rkvc_free(d->y0_d_nhwc);
    rkvc_free(d->y1_d_nhwc);
    rkvc_free(d->x_img);
}

rkvc_err rkvc_mlvc_dec_open(rkvc_mlvc_dec **out, const rkvc_mlvc_dec_config *cfg)
{
    if (!out || !cfg || !cfg->dec_model_path ||
        !cfg->gaussian_pmf_path || !cfg->bitest_pmf_path)
        return RKVC_ERR_INVALID;
    *out = NULL;

    rkvc_mlvc_dec *d = rkvc_calloc(1, sizeof(*d));
    if (!d) return RKVC_ERR_NOMEM;
    d->qp = cfg->qp > 0 ? cfg->qp : 21;

    mlvc_pmf gpmf = {0}, bpmf = {0};
    rkvc_err err = load_pmf(&gpmf, cfg->gaussian_pmf_path);
    if (err) { rkvc_free(d); return err; }
    err = load_pmf(&bpmf, cfg->bitest_pmf_path);
    if (err) { free_pmf(&gpmf); rkvc_free(d); return err; }
    if (!gpmf.index_space) {
        free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(d);
        return RKVC_ERR_FORMAT;
    }

    int rc = rkvc_rans_coder_init(&d->g_coder, RKVC_RANS_BYTE,
                                  gpmf.lengths, gpmf.num_lengths,
                                  gpmf.offsets, gpmf.num_offsets,
                                  gpmf.table, gpmf.num_table, 16, 2);
    if (rc) { free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(d); return RKVC_ERR_INTERNAL; }
    rc = rkvc_rans_coder_init(&d->b_coder, RKVC_RANS_BYTE,
                              bpmf.lengths, bpmf.num_lengths,
                              bpmf.offsets, bpmf.num_offsets,
                              bpmf.table, bpmf.num_table, 16, 2);
    if (rc) { rkvc_rans_coder_free(&d->g_coder); free_pmf(&gpmf); free_pmf(&bpmf); rkvc_free(d); return RKVC_ERR_INTERNAL; }

    free_pmf(&gpmf);
    free_pmf(&bpmf);

    err = rknn_model_init(&d->dec_model, cfg->dec_model_path,
                          cfg->qp_patch_path, d->qp);
    if (err) { rkvc_rans_coder_free(&d->g_coder); rkvc_rans_coder_free(&d->b_coder); rkvc_free(d); return err; }

    err = rknn_alloc_input_mems(&d->dec_model);
    if (err) goto fail_model;
    err = rknn_alloc_output_mems(&d->dec_model);
    if (err) goto fail_model;

    err = dec_resolve_geom(d);
    if (err) goto fail_model;
    err = dec_alloc_bufs(d);
    if (err) goto fail_model;

    *out = d;
    return RKVC_OK;

fail_model:
    dec_free_bufs(d);
    rknn_model_cleanup(&d->dec_model);
    rkvc_rans_coder_free(&d->g_coder);
    rkvc_rans_coder_free(&d->b_coder);
    rkvc_free(d);
    return err;
}

void rkvc_mlvc_dec_close(rkvc_mlvc_dec *d)
{
    if (!d) return;
    rkvc_buffer_unref(d->pending_pkt);
    rkvc_buffer_unref(d->out_frame);
    dec_free_bufs(d);
    rknn_model_cleanup(&d->dec_model);
    rkvc_rans_coder_free(&d->g_coder);
    rkvc_rans_coder_free(&d->b_coder);
    rkvc_free(d);
}

int rkvc_mlvc_dec_width(const rkvc_mlvc_dec *d)  { return d ? d->IMG_W : 0; }
int rkvc_mlvc_dec_height(const rkvc_mlvc_dec *d) { return d ? d->IMG_H : 0; }

rkvc_err rkvc_mlvc_dec_send_packet(rkvc_mlvc_dec *d, const rkvc_buffer *pkt)
{
    if (!d) return RKVC_ERR_INVALID;
    if (d->pending_pkt) return RKVC_ERR_AGAIN;
    if (!pkt) { d->input_eof = 1; return RKVC_OK; }
    if (pkt->kind != RKVC_BUF_BITSTREAM) return RKVC_ERR_FORMAT;
    d->pending_pkt = rkvc_buffer_ref((rkvc_buffer *)pkt);
    return RKVC_OK;
}

rkvc_err rkvc_mlvc_dec_receive_frame(rkvc_mlvc_dec *d, rkvc_buffer **frame)
{
    if (!d || !frame) return RKVC_ERR_INVALID;
    *frame = NULL;
    rkvc_buffer_unref(d->out_frame);
    d->out_frame = NULL;

    if (!d->pending_pkt) {
        if (d->input_eof) return RKVC_ERR_EOF;
        return RKVC_ERR_AGAIN;
    }

    const rkvc_buffer *pkt = d->pending_pkt;

    /* 熵解码 */
    rkvc_rans_dec_stream ds;
    rkvc_rans_dec_stream_init(&ds, RKVC_RANS_BYTE);
    int rc = rkvc_rans_dec_stream_open(&ds, pkt->data, pkt->size);
    if (rc != RKVC_RANS_OK) { RKVC_LOG("dec stream open failed"); return RKVC_ERR_FORMAT; }

    size_t z_n = (size_t)d->ZC * d->ZH * d->ZW;
    size_t y_n = (size_t)d->YC * d->YH * d->YW;

    rc = rkvc_rans_dec_stream_decode(&ds, &d->b_coder, d->z_d, d->z_idx, z_n);
    if (rc != RKVC_RANS_OK) { rkvc_rans_dec_stream_close(&ds); return RKVC_ERR_INTERNAL; }

    mlvc_px_extract_scales(d->z_d, d->s0, d->s1, d->YC, d->YH, d->YW, d->ZH, d->ZW);

    rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y0_d, d->s0, y_n);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y1_d, d->s1, y_n);
    if (rc != RKVC_RANS_OK) { rkvc_rans_dec_stream_close(&ds); return RKVC_ERR_INTERNAL; }

    rkvc_rans_dec_stream_close(&ds);

    /* NCHW → NC1HWC2 fp16 */
    mlvc_px_nchw_to_nc1hwc2_fp16(d->z_d, d->z_d_nhwc, d->ZC, d->ZH, d->ZW);
    mlvc_px_nchw_to_nc1hwc2_fp16(d->y0_d, d->y0_d_nhwc, d->YC, d->YH, d->YW);
    mlvc_px_nchw_to_nc1hwc2_fp16(d->y1_d, d->y1_d_nhwc, d->YC, d->YH, d->YW);

    /* decoder NPU */
    rknn_write_input(&d->dec_model, d->dec_z_in, d->z_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_y0_in, d->y0_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_y1_in, d->y1_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_ref_in, d->ref_nhwc);
    MLVC_RKNN_CHECK(rknn_run(d->dec_model.ctx, NULL));

    const uint16_t *xh = rknn_output_data(&d->dec_model, d->dec_x_out);
    if (!xh)
        return RKVC_ERR_HW;
    rkvc_err err;
    if (d->x_d2s_bs > 0) {
        /* NC1HWC2 → DepthToSpace 融合（免中间 NCHW 遍）；clip(0,1)
         * 融合进饱和 NV12 转换（越界值由整数饱和覆盖）*/
        int C1 = d->x_pre_c / d->OUT_C2;
        mlvc_px_nc1hwc2_d2s_dcr_f16(xh, d->x_img, C1, d->x_pre_h, d->x_pre_w,
                                    d->OUT_C2, d->x_d2s_bs);
        err = nchw_yuv_fp16_to_nv12(d->x_img, d->IMG_W, d->IMG_H, frame);
    } else {
        err = nc1hwc2_fp16_to_nv12(xh, d->IMG_W, d->IMG_H, d->OUT_C2, frame);
    }
    if (err != RKVC_OK) return err;
    (*frame)->pts = pkt->pts;
    (*frame)->key_frame = (d->frame_count == 0);

    /* ref_feature → 下一帧 */
    const uint16_t *fv = rknn_output_data(&d->dec_model, d->dec_ref_out);
    if (!fv)
        return RKVC_ERR_HW;
    memcpy(d->ref_nhwc, fv, (size_t)d->REF_C * d->REF_H * d->REF_W * sizeof(uint16_t));

    rkvc_buffer_unref(d->pending_pkt);
    d->pending_pkt = NULL;
    d->frame_count++;
    return RKVC_OK;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  .mlvc 容器封装/解封装                                               */
/* ════════════════════════════════════════════════════════════════════ */

#define MLVC_MAGIC      "MLVC1"
#define MLVC_MAGIC_LEN  5
#define MLVC_VERSION    1
#define MLVC_HDR_SIZE   32

struct rkvc_mlvc_mux {
    FILE *fp;
    uint32_t frame_count;
    long frame_count_pos;
};

rkvc_err rkvc_mlvc_mux_open(rkvc_mlvc_mux **out, const rkvc_mlvc_mux_config *cfg)
{
    if (!out || !cfg || !cfg->output_path) return RKVC_ERR_INVALID;
    *out = NULL;

    rkvc_mlvc_mux *m = rkvc_calloc(1, sizeof(*m));
    if (!m) return RKVC_ERR_NOMEM;
    m->fp = fopen(cfg->output_path, "wb");
    if (!m->fp) { rkvc_free(m); return RKVC_ERR_IO; }

    uint8_t hdr[MLVC_HDR_SIZE];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, MLVC_MAGIC, MLVC_MAGIC_LEN);
    hdr[5] = MLVC_VERSION;
    uint32_t w = cfg->width, h = cfg->height;
    uint32_t fn = cfg->fps_num > 0 ? (uint32_t)cfg->fps_num : 30;
    uint32_t fd = cfg->fps_den > 0 ? (uint32_t)cfg->fps_den : 1;
    uint32_t qp = (uint32_t)cfg->qp;
    memcpy(hdr + 8, &w, 4);
    memcpy(hdr + 12, &h, 4);
    memcpy(hdr + 16, &fn, 4);
    memcpy(hdr + 20, &fd, 4);
    memcpy(hdr + 24, &qp, 4);
    if (fwrite(hdr, 1, MLVC_HDR_SIZE, m->fp) != MLVC_HDR_SIZE) {
        fclose(m->fp); rkvc_free(m); return RKVC_ERR_IO;
    }
    m->frame_count_pos = 28;
    *out = m;
    return RKVC_OK;
}

void rkvc_mlvc_mux_close(rkvc_mlvc_mux *m)
{
    if (!m) return;
    if (m->fp) {
        if (fseek(m->fp, m->frame_count_pos, SEEK_SET) == 0) {
            uint32_t fc = m->frame_count;
            fwrite(&fc, 4, 1, m->fp);
        }
        fclose(m->fp);
    }
    rkvc_free(m);
}

rkvc_err rkvc_mlvc_mux_write_packet(rkvc_mlvc_mux *m, const rkvc_buffer *pkt)
{
    if (!m || !m->fp || !pkt || pkt->kind != RKVC_BUF_BITSTREAM)
        return RKVC_ERR_INVALID;
    uint32_t sz = (uint32_t)pkt->size;
    uint8_t kf = pkt->key_frame ? 1 : 0;
    uint8_t pad[3] = {0, 0, 0};
    if (fwrite(&sz, 4, 1, m->fp) != 1 ||
        fwrite(&kf, 1, 1, m->fp) != 1 ||
        fwrite(pad, 1, 3, m->fp) != 3 ||
        fwrite(pkt->data, 1, pkt->size, m->fp) != pkt->size)
        return RKVC_ERR_IO;
    m->frame_count++;
    return RKVC_OK;
}

struct rkvc_mlvc_demux {
    FILE *fp;
    int eof;
    int width, height;
    int fps_num, fps_den;
    int qp;
};

rkvc_err rkvc_mlvc_demux_open(rkvc_mlvc_demux **out, const rkvc_mlvc_demux_config *cfg)
{
    if (!out || !cfg || !cfg->input_path) return RKVC_ERR_INVALID;
    *out = NULL;

    rkvc_mlvc_demux *d = rkvc_calloc(1, sizeof(*d));
    if (!d) return RKVC_ERR_NOMEM;
    d->fp = fopen(cfg->input_path, "rb");
    if (!d->fp) { rkvc_free(d); return RKVC_ERR_IO; }

    uint8_t hdr[MLVC_HDR_SIZE];
    if (fread(hdr, 1, MLVC_HDR_SIZE, d->fp) != MLVC_HDR_SIZE) {
        fclose(d->fp); rkvc_free(d); return RKVC_ERR_FORMAT;
    }
    if (memcmp(hdr, MLVC_MAGIC, MLVC_MAGIC_LEN) != 0 || hdr[5] != MLVC_VERSION) {
        fclose(d->fp); rkvc_free(d); return RKVC_ERR_FORMAT;
    }
    uint32_t w, h, fn, fd, qp;
    memcpy(&w, hdr + 8, 4);
    memcpy(&h, hdr + 12, 4);
    memcpy(&fn, hdr + 16, 4);
    memcpy(&fd, hdr + 20, 4);
    memcpy(&qp, hdr + 24, 4);
    d->width = (int)w; d->height = (int)h;
    d->fps_num = (int)fn; d->fps_den = (int)fd; d->qp = (int)qp;
    *out = d;
    return RKVC_OK;
}

void rkvc_mlvc_demux_close(rkvc_mlvc_demux *d)
{
    if (!d) return;
    if (d->fp) fclose(d->fp);
    rkvc_free(d);
}

rkvc_err rkvc_mlvc_demux_read_packet(rkvc_mlvc_demux *d, rkvc_buffer **pkt)
{
    if (!d || !pkt) return RKVC_ERR_INVALID;
    *pkt = NULL;
    if (d->eof) return RKVC_ERR_EOF;

    uint32_t sz;
    uint8_t kf, pad[3];
    if (fread(&sz, 4, 1, d->fp) != 1) { d->eof = 1; return RKVC_ERR_EOF; }
    if (fread(&kf, 1, 1, d->fp) != 1 || fread(pad, 1, 3, d->fp) != 3) {
        d->eof = 1; return RKVC_ERR_IO;
    }
    if (sz == 0 || sz > 64 * 1024 * 1024) return RKVC_ERR_FORMAT;

    uint8_t *buf = rkvc_malloc(sz);
    if (!buf) return RKVC_ERR_NOMEM;
    if (fread(buf, 1, sz, d->fp) != sz) {
        rkvc_free(buf); d->eof = 1; return RKVC_ERR_IO;
    }

    rkvc_buffer *b = NULL;
    rkvc_err err = rkvc_buffer_alloc_bitstream(&b, buf, sz, 1);
    rkvc_free(buf);
    if (err != RKVC_OK) return err;
    b->key_frame = kf;
    *pkt = b;
    return RKVC_OK;
}

int rkvc_mlvc_demux_qp(const rkvc_mlvc_demux *d)
{
    return d ? d->qp : 21;
}

#endif /* RKVC_ENABLE_MLVC */
