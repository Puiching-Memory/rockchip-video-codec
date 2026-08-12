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
#include "rans.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <rknn_api.h>

/* ── fp16 辅助（软件实现，可移植）────────────────────────────────── */

static uint16_t f32_to_f16(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000;
        int shift = 14 - exp;
        uint32_t hm = mant >> shift;
        uint32_t rem = mant & ((1u << shift) - 1);
        uint32_t half_ = 1u << (shift - 1);
        if (rem > half_ || (rem == half_ && (hm & 1))) hm++;
        return (uint16_t)(sign | hm);
    }
    if (exp >= 31) return (uint16_t)(sign | 0x7bff);
    uint32_t h = sign | ((uint32_t)exp << 10) | (mant >> 13);
    uint32_t rem = mant & 0x1fff;
    if (rem > 0x1000 || (rem == 0x1000 && (h & 1))) h++;
    return (uint16_t)h;
}

static float f16_to_f32(uint16_t h)
{
    uint32_t sign = ((uint32_t)h & 0x8000) << 16;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t x;
    if (exp == 0) {
        if (mant == 0) {
            x = sign;
        } else {
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        x = sign | 0x7f800000u | (mant << 13);
    } else {
        x = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float f;
    memcpy(&f, &x, 4);
    return f;
}

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

static rkvc_err load_pmf(mlvc_pmf *p, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return RKVC_ERR_IO;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PMF1", 4) != 0) {
        fclose(f);
        return RKVC_ERR_FORMAT;
    }

    memset(p, 0, sizeof(*p));
    uint32_t nL, nO, nT;
    if (fread(&nL, 4, 1, f) != 1 || fread(&nO, 4, 1, f) != 1 ||
        fread(&nT, 4, 1, f) != 1) {
        fclose(f);
        return RKVC_ERR_FORMAT;
    }
    p->num_lengths = nL;
    p->num_offsets = nO;
    p->num_table = nT;
    p->lengths = rkvc_malloc(nL * 4);
    p->offsets = rkvc_malloc(nO * 4);
    p->table   = rkvc_malloc(nT * 4);
    if (!p->lengths || !p->offsets || !p->table) {
        fclose(f);
        return RKVC_ERR_NOMEM;
    }
    if (fread(p->lengths, 4, nL, f) != nL ||
        fread(p->offsets, 4, nO, f) != nO ||
        fread(p->table, 4, nT, f) != nT) {
        fclose(f);
        return RKVC_ERR_FORMAT;
    }
    uint32_t tag = 0;
    if (fread(&tag, 4, 1, f) != 1) {
        fclose(f);
        return RKVC_ERR_FORMAT;
    }
    if (tag == 1) {
        if (fread(&p->scale_min, 8, 1, f) != 1 ||
            fread(&p->scale_max, 8, 1, f) != 1 ||
            fread(&p->scale_levels, 4, 1, f) != 1 ||
            fread(&p->index_space, 4, 1, f) != 1) {
            fclose(f);
            return RKVC_ERR_FORMAT;
        }
    } else if (tag == 2) {
        if (fread(&p->qp_num, 4, 1, f) != 1 ||
            fread(&p->channels, 4, 1, f) != 1) {
            fclose(f);
            return RKVC_ERR_FORMAT;
        }
    } else {
        fclose(f);
        return RKVC_ERR_FORMAT;
    }
    fclose(f);
    return RKVC_OK;
}

static void free_pmf(mlvc_pmf *p)
{
    rkvc_free(p->lengths);
    rkvc_free(p->offsets);
    rkvc_free(p->table);
    memset(p, 0, sizeof(*p));
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

static rkvc_err rknn_model_init(mlvc_rknn_model *m, const char *path)
{
    memset(m, 0, sizeof(*m));

    FILE *fp = fopen(path, "rb");
    if (!fp)
        return RKVC_ERR_IO;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    m->model_buf = rkvc_malloc(sz);
    if (!m->model_buf) {
        fclose(fp);
        return RKVC_ERR_NOMEM;
    }
    if (fread(m->model_buf, 1, sz, fp) != (size_t)sz) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    fclose(fp);
    m->model_size = sz;

    /* 分发构建：在 rknn_init 前强制 librknnrt 静默，避免 RKNN_LOG_LEVEL
     * 泄露已解密模型的网络结构（见 rkvc_rknn_quiet_runtime 说明）。 */
    rkvc_rknn_quiet_runtime();
    MLVC_RKNN_CHECK(rknn_init(&m->ctx, m->model_buf, sz, 0, NULL));
    MLVC_RKNN_CHECK(rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io_num, sizeof(m->io_num)));

    for (uint32_t i = 0; i < m->io_num.n_input; i++) {
        m->in_attr[i].index = i;
        MLVC_RKNN_CHECK(rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &m->in_attr[i], sizeof(m->in_attr[i])));
    }
    for (uint32_t i = 0; i < m->io_num.n_output; i++) {
        m->out_attr[i].index = i;
        MLVC_RKNN_CHECK(rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &m->out_attr[i], sizeof(m->out_attr[i])));
    }
    return RKVC_OK;
}

static void rknn_model_cleanup(mlvc_rknn_model *m)
{
    for (uint32_t i = 0; i < m->io_num.n_input; i++)
        if (m->in_mem[i]) rknn_destroy_mem(m->ctx, m->in_mem[i]);
    for (uint32_t i = 0; i < m->io_num.n_output; i++)
        if (m->out_mem[i]) rknn_destroy_mem(m->ctx, m->out_mem[i]);
    if (m->ctx) rknn_destroy(m->ctx);
    rkvc_free(m->model_buf);
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

static const uint16_t *rknn_output_data(mlvc_rknn_model *m, uint32_t i)
{
    rknn_mem_sync(m->ctx, m->out_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
    return (const uint16_t *)m->out_mem[i]->virt_addr;
}

static void rknn_write_input(mlvc_rknn_model *m, uint32_t i, const uint16_t *data)
{
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

/* ── 几何常量 ────────────────────────────────────────────────────── */

#define MLVC_SCALE_MAX_IDX     127
#define MLVC_CHANNEL_REPEAT    4
#define MLVC_SPATIAL_REPEAT    4

/* ── extract_scales ── */

static void extract_scales(const int32_t *z_r, int32_t *s0, int32_t *s1,
                           int YC, int YH, int YW, int ZH, int ZW)
{
    for (int c = 0; c < YC; c++) {
        int zc0 = c / MLVC_CHANNEL_REPEAT;
        int zc1 = (c + YC) / MLVC_CHANNEL_REPEAT;
        for (int y = 0; y < YH; y++) {
            int zy = y / MLVC_SPATIAL_REPEAT;
            for (int x = 0; x < YW; x++) {
                int zx = x / MLVC_SPATIAL_REPEAT;
                int v0 = abs(z_r[(zc0 * ZH + zy) * ZW + zx]);
                int v1 = abs(z_r[(zc1 * ZH + zy) * ZW + zx]);
                if (v0 > MLVC_SCALE_MAX_IDX) v0 = MLVC_SCALE_MAX_IDX;
                if (v1 > MLVC_SCALE_MAX_IDX) v1 = MLVC_SCALE_MAX_IDX;
                int chk = ((y & 1) == (x & 1));
                size_t o = ((size_t)c * YH + y) * YW + x;
                s0[o] = chk ? v0 : v1;
                s1[o] = chk ? v1 : v0;
            }
        }
    }
}

/* ── NC1HWC2 ↔ NCHW 转换 ── */

static void nc1hwc2_to_nchw(const uint16_t *src, int32_t *dst, int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] =
                    lrintf(f16_to_f32(src[(((size_t)c1 * H + y) * W + x) * 8 + c2]));
    }
}

static void nchw_to_nc1hwc2_fp16(const int32_t *src, uint16_t *dst, int C, int H, int W)
{
    for (int c = 0; c < C; c++) {
        int c1 = c >> 3, c2 = c & 7;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[(((size_t)c1 * H + y) * W + x) * 8 + c2] =
                    f32_to_f16((float)src[((size_t)c * H + y) * W + x]);
    }
}

/* ── YUV → fp16 NHWC ── */

static void yuv_to_nhwc_fp16(const rkvc_buffer *frame, uint16_t *nhwc)
{
    AVFrame *avf = frame->av_frame;
    int W = (int)frame->width;
    int H = (int)frame->height;
    const uint8_t *Yp = avf->data[0];
    int y_stride = avf->linesize[0];
    int is_nv12 = (frame->format == RKVC_PIX_FMT_NV12);
    const uint8_t *Up, *Vp;
    int uv_stride;
    if (is_nv12) {
        Up = avf->data[1];
        Vp = Up + 1;
        uv_stride = avf->linesize[1];
    } else {
        Up = avf->data[1];
        Vp = avf->data[2];
        uv_stride = avf->linesize[1];
    }
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t o = (size_t)y * W + x;
            int uy = y / 2, ux = x / 2;
            nhwc[o * 3 + 0] = f32_to_f16(Yp[y * y_stride + x] * (1.0f / 255.0f));
            if (is_nv12) {
                nhwc[o * 3 + 1] = f32_to_f16(Up[uy * uv_stride + ux * 2] * (1.0f / 255.0f));
                nhwc[o * 3 + 2] = f32_to_f16(Vp[uy * uv_stride + ux * 2] * (1.0f / 255.0f));
            } else {
                nhwc[o * 3 + 1] = f32_to_f16(Up[uy * uv_stride + ux] * (1.0f / 255.0f));
                nhwc[o * 3 + 2] = f32_to_f16(Vp[uy * uv_stride + ux] * (1.0f / 255.0f));
            }
        }
    }
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
    uint8_t *Yp = avf->data[0];
    uint8_t *UV = avf->data[1];
    int y_stride = avf->linesize[0];
    int uv_stride = avf->linesize[1];

    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            size_t o = ((size_t)y * W + x) * c2;
            int v = (int)lrintf(f16_to_f32(src[o + 0]) * 255.0f);
            Yp[y * y_stride + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
    for (int y = 0; y < H / 2; y++) {
        for (int x = 0; x < W / 2; x++) {
            size_t o = ((size_t)(y * 2) * W + (x * 2)) * c2;
            int u = (int)lrintf(f16_to_f32(src[o + 1]) * 255.0f);
            int v = (int)lrintf(f16_to_f32(src[o + 2]) * 255.0f);
            UV[y * uv_stride + x * 2]     = (uint8_t)(u < 0 ? 0 : (u > 255 ? 255 : u));
            UV[y * uv_stride + x * 2 + 1] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
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
    int enc_feat_out, enc_z_out, enc_y0_out;

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
    e->IMG_H = enc->in_attr[0].dims[1];
    e->IMG_W = enc->in_attr[0].dims[2];
    e->REF_H = enc->in_attr[1].dims[1];
    e->REF_W = enc->in_attr[1].dims[2];
    e->REF_C = enc->in_attr[1].dims[3];

    e->enc_feat_out = rknn_find_output(enc, "feature");
    e->enc_z_out    = rknn_find_output(enc, "z_raw");
    e->enc_y0_out   = rknn_find_output(enc, "y_raw_0");
    if (e->enc_feat_out < 0) e->enc_feat_out = 0;
    if (e->enc_z_out < 0 || e->enc_y0_out < 0)
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
    for (int c = 0; c < e->ZC; c++)
        for (int y = 0; y < e->ZH; y++)
            for (int x = 0; x < e->ZW; x++)
                e->z_idx[((size_t)c * e->ZH + y) * e->ZW + x] = e->qp * e->ZC + c;
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
    mlvc_pmf gpmf, bpmf;
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
    err = rknn_model_init(&e->enc_model, cfg->enc_model_path);
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
    rknn_write_input(&e->enc_model, 0, e->x_nhwc);
    rknn_write_input(&e->enc_model, 1, e->ref_nhwc);
    MLVC_RKNN_CHECK(rknn_run(e->enc_model.ctx, NULL));

    const uint16_t *bz  = rknn_output_data(&e->enc_model, e->enc_z_out);
    const uint16_t *by0 = rknn_output_data(&e->enc_model, e->enc_y0_out);
    const uint16_t *by1 = rknn_output_data(&e->enc_model, e->enc_y0_out + 1);

    nc1hwc2_to_nchw(bz, e->z_r, e->ZC, e->ZH, e->ZW);
    nc1hwc2_to_nchw(by0, e->y0_r, e->YC, e->YH, e->YW);
    nc1hwc2_to_nchw(by1, e->y1_r, e->YC, e->YH, e->YW);

    /* 熵编码: push y1, y0, z */
    extract_scales(e->z_r, e->s0, e->s1, e->YC, e->YH, e->YW, e->ZH, e->ZW);

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
    int ZC, ZH, ZW, YC, YH, YW;
    int dec_z_in, dec_y0_in, dec_ref_in, dec_ref_out;

    uint16_t *ref_nhwc;
    int32_t *z_d, *y0_d, *y1_d, *s0, *s1, *z_idx;
    uint16_t *z_d_nhwc, *y0_d_nhwc, *y1_d_nhwc;

    rkvc_buffer *pending_pkt;
    rkvc_buffer *out_frame;
    int frame_count;
    int input_eof;
};

static rkvc_err dec_resolve_geom(rkvc_mlvc_dec *d)
{
    mlvc_rknn_model *dec = &d->dec_model;
    d->dec_z_in  = 0;
    d->dec_y0_in = 1;
    d->dec_ref_in = (int)dec->io_num.n_input - 1;
    d->dec_ref_out = 1;

    d->IMG_H = dec->native_out_attr[0].dims[2];
    d->IMG_W = dec->native_out_attr[0].dims[3];
    d->OUT_C2 = (dec->native_out_attr[0].n_dims >= 5)
              ? dec->native_out_attr[0].dims[4] : 8;
    if (d->OUT_C2 < 3) d->OUT_C2 = 3;
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
    if (!d->ref_nhwc || !d->z_d || !d->y0_d || !d->y1_d || !d->s0 || !d->s1 ||
        !d->z_idx || !d->z_d_nhwc || !d->y0_d_nhwc || !d->y1_d_nhwc)
        return RKVC_ERR_NOMEM;

    memset(d->ref_nhwc, 0, ref_sz * 2);
    for (int c = 0; c < d->ZC; c++)
        for (int y = 0; y < d->ZH; y++)
            for (int x = 0; x < d->ZW; x++)
                d->z_idx[((size_t)c * d->ZH + y) * d->ZW + x] = d->qp * d->ZC + c;
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

    mlvc_pmf gpmf, bpmf;
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

    err = rknn_model_init(&d->dec_model, cfg->dec_model_path);
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

    extract_scales(d->z_d, d->s0, d->s1, d->YC, d->YH, d->YW, d->ZH, d->ZW);

    rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y0_d, d->s0, y_n);
    if (rc == RKVC_RANS_OK)
        rc = rkvc_rans_dec_stream_decode(&ds, &d->g_coder, d->y1_d, d->s1, y_n);
    if (rc != RKVC_RANS_OK) { rkvc_rans_dec_stream_close(&ds); return RKVC_ERR_INTERNAL; }

    rkvc_rans_dec_stream_close(&ds);

    /* NCHW → NC1HWC2 fp16 */
    nchw_to_nc1hwc2_fp16(d->z_d, d->z_d_nhwc, d->ZC, d->ZH, d->ZW);
    nchw_to_nc1hwc2_fp16(d->y0_d, d->y0_d_nhwc, d->YC, d->YH, d->YW);
    nchw_to_nc1hwc2_fp16(d->y1_d, d->y1_d_nhwc, d->YC, d->YH, d->YW);

    /* decoder NPU */
    rknn_write_input(&d->dec_model, 0, d->z_d_nhwc);
    rknn_write_input(&d->dec_model, 1, d->y0_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_y0_in + 1, d->y1_d_nhwc);
    rknn_write_input(&d->dec_model, d->dec_ref_in, d->ref_nhwc);
    MLVC_RKNN_CHECK(rknn_run(d->dec_model.ctx, NULL));

    const uint16_t *xh = rknn_output_data(&d->dec_model, 0);
    rkvc_err err = nc1hwc2_fp16_to_nv12(xh, d->IMG_W, d->IMG_H, d->OUT_C2, frame);
    if (err != RKVC_OK) return err;
    (*frame)->pts = pkt->pts;
    (*frame)->key_frame = (d->frame_count == 0);

    /* ref_feature → 下一帧 */
    const uint16_t *fv = rknn_output_data(&d->dec_model, d->dec_ref_out);
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
