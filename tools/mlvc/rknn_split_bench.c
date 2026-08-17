/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆
 *
 * MLVC 图边界算子外提 A/B：输出必须逐字节一致，且含 CPU 外提的墙钟更快才算过。
 *
 *   --exp enc_std     A=整图  B=无 SpaceToDepth（CPU unshuffle 后喂 NPU）
 *   --exp dec_d2s     A=整图  B=无尾部 DepthToSpace+Clip（NPU 出 head，CPU shuffle+clip）
 *   --exp custom_std  同一 .rknn 两次：B 注册自定义 SpaceToDepth
 */
#define _POSIX_C_SOURCE 200809L
#include <rknn_api.h>
#include <rknn_custom_op.h>

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_IO 8
#define CHECK(x)                                                               \
    do {                                                                       \
        int _rc = (x);                                                         \
        if (_rc != RKNN_SUCC) {                                                \
            fprintf(stderr, "RKNN error %d at %s:%d\n", _rc, __FILE__,         \
                    __LINE__);                                                 \
            return -1;                                                         \
        }                                                                      \
    } while (0)

static inline uint16_t f32_to_f16(float f)
{
    __fp16 h = (__fp16)f;
    uint16_t r;
    memcpy(&r, &h, 2);
    return r;
}

static inline float f16_to_f32(uint16_t h)
{
    __fp16 p;
    memcpy(&p, &h, 2);
    return (float)p;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int g_custom_hits;

typedef struct {
    rknn_context ctx;
    rknn_input_output_num io;
    rknn_tensor_attr in_attr[MAX_IO];
    rknn_tensor_attr out_attr[MAX_IO];
    rknn_tensor_attr native_out[MAX_IO];
    rknn_tensor_mem *in_mem[MAX_IO];
    rknn_tensor_mem *out_mem[MAX_IO];
    void *model_buf;
    uint32_t model_size;
    int custom;
} model_t;

static int find_name(const rknn_tensor_attr *a, uint32_t n, const char *key)
{
    for (uint32_t i = 0; i < n; i++)
        if (strstr(a[i].name, key))
            return (int)i;
    return -1;
}

static void space_to_depth_nhwc_f16(const uint16_t *in, uint16_t *out, int H,
                                    int W, int C, int bs)
{
    int oh = H / bs, ow = W / bs, oc = C * bs * bs;
    for (int h = 0; h < oh; h++) {
        for (int w = 0; w < ow; w++) {
            for (int c = 0; c < C; c++) {
                for (int dy = 0; dy < bs; dy++) {
                    for (int dx = 0; dx < bs; dx++) {
                        int oci = c * bs * bs + dy * bs + dx;
                        out[(h * ow + w) * oc + oci] =
                            in[((h * bs + dy) * W + (w * bs + dx)) * C + c];
                    }
                }
            }
        }
    }
}

static void space_to_depth_nchw_f16(const uint16_t *in, uint16_t *out, int C,
                                    int H, int W, int bs)
{
    int oh = H / bs, ow = W / bs;
    for (int c = 0; c < C; c++) {
        for (int dy = 0; dy < bs; dy++) {
            for (int dx = 0; dx < bs; dx++) {
                int oci = c * bs * bs + dy * bs + dx;
                for (int h = 0; h < oh; h++) {
                    for (int w = 0; w < ow; w++) {
                        out[(oci * oh + h) * ow + w] =
                            in[(c * H + h * bs + dy) * W + (w * bs + dx)];
                    }
                }
            }
        }
    }
}

static void depth_to_space_nchw_crd_f16(const uint16_t *in, uint16_t *out, int C,
                                        int H, int W, int bs)
{
    int oc = C / (bs * bs);
    int oh = H * bs, ow = W * bs;
    for (int c = 0; c < oc; c++) {
        for (int dy = 0; dy < bs; dy++) {
            for (int dx = 0; dx < bs; dx++) {
                int ic = c * bs * bs + dy * bs + dx;
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        out[(c * oh + h * bs + dy) * ow + (w * bs + dx)] =
                            in[(ic * H + h) * W + w];
                    }
                }
            }
        }
    }
}

static void depth_to_space_nchw_dcr_f16(const uint16_t *in, uint16_t *out, int C,
                                        int H, int W, int bs)
{
    int oc = C / (bs * bs);
    int oh = H * bs, ow = W * bs;
    for (int c = 0; c < oc; c++) {
        for (int dy = 0; dy < bs; dy++) {
            for (int dx = 0; dx < bs; dx++) {
                int ic = dy * (bs * oc) + dx * oc + c;
                for (int h = 0; h < H; h++) {
                    for (int w = 0; w < W; w++) {
                        out[(c * oh + h * bs + dy) * ow + (w * bs + dx)] =
                            in[(ic * H + h) * W + w];
                    }
                }
            }
        }
    }
}

static void space_to_depth_nchw_f32(const float *in, float *out, int C, int H,
                                    int W, int bs)
{
    int oh = H / bs, ow = W / bs;
    for (int c = 0; c < C; c++) {
        for (int dy = 0; dy < bs; dy++) {
            for (int dx = 0; dx < bs; dx++) {
                int oci = c * bs * bs + dy * bs + dx;
                for (int h = 0; h < oh; h++) {
                    for (int w = 0; w < ow; w++) {
                        out[(oci * oh + h) * ow + w] =
                            in[(c * H + h * bs + dy) * W + (w * bs + dx)];
                    }
                }
            }
        }
    }
}

static void space_to_depth_nhwc_f32(const float *in, float *out, int H, int W,
                                    int C, int bs)
{
    int oh = H / bs, ow = W / bs, oc = C * bs * bs;
    for (int h = 0; h < oh; h++) {
        for (int w = 0; w < ow; w++) {
            for (int c = 0; c < C; c++) {
                for (int dy = 0; dy < bs; dy++) {
                    for (int dx = 0; dx < bs; dx++) {
                        int oci = c * bs * bs + dy * bs + dx;
                        out[(h * ow + w) * oc + oci] =
                            in[((h * bs + dy) * W + (w * bs + dx)) * C + c];
                    }
                }
            }
        }
    }
}

static void clip01_f16_inplace(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float f = f16_to_f32(p[i]);
        if (f <= 0.0f)
            p[i] = f32_to_f16(0.0f);
        else if (f >= 1.0f)
            p[i] = f32_to_f16(1.0f);
        /* in-range: keep bits */
    }
}

static void nc1hwc2_to_nchw_f16(const uint16_t *src, uint16_t *dst, int C1,
                                int H, int W, int C2)
{
    int C = C1 * C2;
    for (int c = 0; c < C; c++) {
        int c1 = c / C2, c2 = c % C2;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                dst[((size_t)c * H + y) * W + x] =
                    src[(((size_t)c1 * H + y) * W + x) * C2 + c2];
    }
}

static int custom_std_compute(rknn_custom_op_context *op_ctx,
                              rknn_custom_op_tensor *inputs, uint32_t n_inputs,
                              rknn_custom_op_tensor *outputs,
                              uint32_t n_outputs)
{
    (void)op_ctx;
    if (n_inputs < 1 || n_outputs < 1)
        return -1;
    g_custom_hits++;
    rknn_tensor_attr *ia = &inputs[0].attr;
    rknn_tensor_attr *oa = &outputs[0].attr;
    const void *in = inputs[0].mem.virt_addr;
    void *out = outputs[0].mem.virt_addr;
    int bs = 8;
    rknn_custom_op_attr attr;
    memset(&attr, 0, sizeof(attr));
    rknn_custom_op_get_op_attr(op_ctx, "blocksize", &attr);
    if (attr.n_elems >= 1 && attr.data) {
        if (attr.dtype == RKNN_TENSOR_INT32)
            bs = *(int32_t *)attr.data;
        else if (attr.dtype == RKNN_TENSOR_INT64)
            bs = (int)*(int64_t *)attr.data;
    }
    if (g_custom_hits == 1) {
        fprintf(stderr,
                "custom SpaceToDepth bs=%d in fmt=%s n_dims=%u dims=%u,%u,%u,%u "
                "type=%s n_elems=%u | out fmt=%s n_dims=%u dims=%u,%u,%u,%u n_elems=%u\n",
                bs, get_format_string(ia->fmt), ia->n_dims, ia->dims[0],
                ia->dims[1], ia->dims[2], ia->dims[3], get_type_string(ia->type),
                ia->n_elems, get_format_string(oa->fmt), oa->n_dims,
                oa->dims[0], oa->dims[1], oa->dims[2], oa->dims[3], oa->n_elems);
    }
    if (ia->type == RKNN_TENSOR_FLOAT16) {
        if (ia->fmt == RKNN_TENSOR_NHWC) {
            int H = (int)ia->dims[1], W = (int)ia->dims[2], C = (int)ia->dims[3];
            space_to_depth_nhwc_f16((const uint16_t *)in, (uint16_t *)out, H, W, C,
                                    bs);
        } else {
            int C = (int)ia->dims[1], H = (int)ia->dims[2], W = (int)ia->dims[3];
            space_to_depth_nchw_f16((const uint16_t *)in, (uint16_t *)out, C, H, W,
                                    bs);
        }
    } else {
        const float *fin = (const float *)in;
        float *fout = (float *)out;
        if (ia->fmt == RKNN_TENSOR_NHWC) {
            int H = (int)ia->dims[1], W = (int)ia->dims[2], C = (int)ia->dims[3];
            space_to_depth_nhwc_f32(fin, fout, H, W, C, bs);
        } else {
            int C = (int)ia->dims[1], H = (int)ia->dims[2], W = (int)ia->dims[3];
            space_to_depth_nchw_f32(fin, fout, C, H, W, bs);
        }
    }
    (void)oa;
    return 0;
}

static int register_std(rknn_context ctx)
{
    rknn_custom_op op;
    memset(&op, 0, sizeof(op));
    op.version = 1;
    op.target = RKNN_TARGET_TYPE_CPU;
    snprintf(op.op_type, sizeof(op.op_type), "SpaceToDepth");
    op.compute = custom_std_compute;
    return rknn_register_custom_ops(ctx, &op, 1);
}

static void model_free(model_t *m)
{
    if (!m)
        return;
    for (uint32_t i = 0; i < MAX_IO; i++) {
        if (m->in_mem[i])
            rknn_destroy_mem(m->ctx, m->in_mem[i]);
        if (m->out_mem[i])
            rknn_destroy_mem(m->ctx, m->out_mem[i]);
    }
    if (m->ctx)
        rknn_destroy(m->ctx);
    free(m->model_buf);
    memset(m, 0, sizeof(*m));
}

static int model_load(model_t *m, const char *path, int custom)
{
    memset(m, 0, sizeof(*m));
    m->custom = custom;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long sz = ftell(fp);
    if (sz <= 0) {
        fclose(fp);
        return -1;
    }
    rewind(fp);
    m->model_buf = malloc((size_t)sz);
    if (!m->model_buf) {
        fclose(fp);
        return -1;
    }
    if (fread(m->model_buf, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        free(m->model_buf);
        return -1;
    }
    fclose(fp);
    m->model_size = (uint32_t)sz;
    CHECK(rknn_init(&m->ctx, m->model_buf, m->model_size, 0, NULL));
    rknn_set_core_mask(m->ctx, RKNN_NPU_CORE_0_1_2);
    if (custom) {
        int rc = register_std(m->ctx);
        printf("  custom SpaceToDepth register rc=%d\n", rc);
        if (rc != RKNN_SUCC)
            fprintf(stderr, "warning: custom op register failed\n");
    }
    CHECK(rknn_query(m->ctx, RKNN_QUERY_IN_OUT_NUM, &m->io, sizeof(m->io)));
    if (m->io.n_input > MAX_IO || m->io.n_output > MAX_IO) {
        fprintf(stderr, "too many IO %u/%u\n", m->io.n_input, m->io.n_output);
        return -1;
    }
    for (uint32_t i = 0; i < m->io.n_input; i++) {
        m->in_attr[i].index = i;
        CHECK(rknn_query(m->ctx, RKNN_QUERY_INPUT_ATTR, &m->in_attr[i],
                         sizeof(m->in_attr[i])));
        rknn_tensor_attr a = m->in_attr[i];
        a.index = i;
        a.type = RKNN_TENSOR_FLOAT16;
        a.fmt = RKNN_TENSOR_NHWC;
        a.pass_through = 1;
        a.size = a.size_with_stride;
        m->in_mem[i] = rknn_create_mem(m->ctx, a.size_with_stride);
        if (!m->in_mem[i])
            return -1;
        CHECK(rknn_set_io_mem(m->ctx, m->in_mem[i], &a));
        printf("  IN[%u] %-24s fmt=%s dims=%u,%u,%u,%u n_elems=%u\n", i,
               a.name, get_format_string(m->in_attr[i].fmt),
               m->in_attr[i].dims[0], m->in_attr[i].dims[1],
               m->in_attr[i].dims[2], m->in_attr[i].dims[3],
               m->in_attr[i].n_elems);
    }
    for (uint32_t i = 0; i < m->io.n_output; i++) {
        m->out_attr[i].index = i;
        CHECK(rknn_query(m->ctx, RKNN_QUERY_OUTPUT_ATTR, &m->out_attr[i],
                         sizeof(m->out_attr[i])));
        rknn_tensor_attr a;
        memset(&a, 0, sizeof(a));
        a.index = i;
        CHECK(rknn_query(m->ctx, RKNN_QUERY_NATIVE_NC1HWC2_OUTPUT_ATTR, &a,
                         sizeof(a)));
        a.index = i;
        a.pass_through = 1;
        m->native_out[i] = a;
        m->out_mem[i] = rknn_create_mem(m->ctx, a.size_with_stride);
        if (!m->out_mem[i])
            return -1;
        CHECK(rknn_set_io_mem(m->ctx, m->out_mem[i], &a));
        printf("  OUT[%u] %-24s native C1=%u H=%u W=%u C2=%u\n", i,
               m->out_attr[i].name, a.dims[1], a.dims[2], a.dims[3],
               a.n_dims >= 5 ? a.dims[4] : 8);
    }
    return 0;
}

static void write_in(model_t *m, int i, const uint16_t *data, size_t n_elem)
{
    memcpy(m->in_mem[i]->virt_addr, data, n_elem * 2);
    rknn_mem_sync(m->ctx, m->in_mem[i], RKNN_MEMORY_SYNC_TO_DEVICE);
}

static const uint16_t *read_out(model_t *m, int i)
{
    rknn_mem_sync(m->ctx, m->out_mem[i], RKNN_MEMORY_SYNC_FROM_DEVICE);
    return (const uint16_t *)m->out_mem[i]->virt_addr;
}

static void fill_pat(uint16_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        p[i] = f32_to_f16((float)((i * 17u + 31u) % 240 + 8) / 255.0f);
}

static int native_geom(const rknn_tensor_attr *a, int *C1, int *H, int *W,
                       int *C2)
{
    *C1 = (int)a->dims[1];
    *H = (int)a->dims[2];
    *W = (int)a->dims[3];
    *C2 = a->n_dims >= 5 ? (int)a->dims[4] : 8;
    return *C1 * *C2 * *H * *W;
}

static int pack_out(model_t *m, int i, uint16_t *dst, int *C, int *H, int *W)
{
    int C1, C2;
    native_geom(&m->native_out[i], &C1, H, W, &C2);
    *C = C1 * C2;
    nc1hwc2_to_nchw_f16(read_out(m, i), dst, C1, *H, *W, C2);
    return *C * *H * *W;
}

static int compare_nchw(const uint16_t *a, const uint16_t *b, int C, int H,
                        int W, int valid_c, const char *tag)
{
    size_t n = (size_t)valid_c * H * W;
    size_t diff = 0;
    float max_abs = 0;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            diff++;
            float d = fabsf(f16_to_f32(a[i]) - f16_to_f32(b[i]));
            if (d > max_abs)
                max_abs = d;
        }
    }
    (void)C;
    if (diff == 0) {
        printf("  MATCH %s  %d x %d x %d  bytes=%zu\n", tag, valid_c, H, W,
               n * 2);
        return 0;
    }
    printf("  MISMATCH %s  %zu / %zu  max_abs=%.6g\n", tag, diff, n, max_abs);
    return 1;
}

static int run_model(model_t *m)
{
    CHECK(rknn_run(m->ctx, NULL));
    return 0;
}

static int nhwc_c(const rknn_tensor_attr *a)
{
    /* node_mlvc 按 NHWC 写：dims[1]=H dims[2]=W dims[3]=C；若 fmt 已是 NHWC 同此。 */
    if (a->n_dims >= 4)
        return (int)a->dims[3];
    return 0;
}

static int nhwc_h(const rknn_tensor_attr *a) { return (int)a->dims[1]; }
static int nhwc_w(const rknn_tensor_attr *a) { return (int)a->dims[2]; }

typedef struct {
    int match;
    int custom_hits;
    double a_ms;
    double b_ms;
    double a_std;
    double b_std;
} bench_result;

static void stats(const double *v, int n, double *mean, double *sd)
{
    double s = 0, q = 0;
    for (int i = 0; i < n; i++)
        s += v[i];
    *mean = s / n;
    for (int i = 0; i < n; i++) {
        double d = v[i] - *mean;
        q += d * d;
    }
    *sd = n > 1 ? sqrt(q / (n - 1)) : 0;
}

static int bench_enc_std(model_t *A, model_t *B, int warmup, int frames,
                         bench_result *out)
{
    int ax = find_name(A->in_attr, A->io.n_input, "x");
    int ar = find_name(A->in_attr, A->io.n_input, "ref_feature");
    int bx = find_name(B->in_attr, B->io.n_input, "x");
    int br = find_name(B->in_attr, B->io.n_input, "ref_feature");
    if (ax < 0 || ar < 0 || bx < 0 || br < 0) {
        fprintf(stderr, "missing x/ref_feature\n");
        return -1;
    }
    int H = nhwc_h(&A->in_attr[ax]), W = nhwc_w(&A->in_attr[ax]),
        C = nhwc_c(&A->in_attr[ax]);
    int bH = nhwc_h(&B->in_attr[bx]), bW = nhwc_w(&B->in_attr[bx]),
        bC = nhwc_c(&B->in_attr[bx]);
    int bs = 8;
    if (H / bH != W / bW || (H / bH) < 2) {
        fprintf(stderr, "unexpected geom A %dx%dx%d  B %dx%dx%d\n", H, W, C, bH,
                bW, bC);
        return -1;
    }
    bs = H / bH;
    printf("  SpaceToDepth CPU bs=%d  %dx%dx%d → %dx%dx%d\n", bs, H, W, C, bH,
           bW, bC);

    size_t img_n = (size_t)H * W * C;
    size_t std_n = (size_t)bH * bW * bC;
    size_t ref_n = A->in_attr[ar].n_elems;
    uint16_t *img = malloc(img_n * 2);
    uint16_t *std = malloc(std_n * 2);
    uint16_t *ref = malloc(ref_n * 2);
    if (!img || !std || !ref)
        return -1;
    fill_pat(img, img_n);
    memset(ref, 0, ref_n * 2);
    space_to_depth_nhwc_f16(img, std, H, W, C, bs);

    write_in(A, ax, img, img_n);
    write_in(A, ar, ref, ref_n);
    write_in(B, bx, std, std_n);
    write_in(B, br, ref, B->in_attr[br].n_elems);

    for (int i = 0; i < warmup; i++) {
        if (run_model(A) || run_model(B))
            return -1;
    }

    int mismatch = 0;
    uint16_t *pa = malloc(16 * 1024 * 1024);
    uint16_t *pb = malloc(16 * 1024 * 1024);
    if (!pa || !pb)
        return -1;
    if (run_model(A) || run_model(B))
        return -1;
    if (A->io.n_output != B->io.n_output) {
        fprintf(stderr, "output count %u vs %u\n", A->io.n_output, B->io.n_output);
        mismatch = 1;
    }
    for (uint32_t i = 0; i < A->io.n_output && !mismatch; i++) {
        int Ca, Ha, Wa, Cb, Hb, Wb;
        pack_out(A, (int)i, pa, &Ca, &Ha, &Wa);
        int bi = find_name(B->out_attr, B->io.n_output, A->out_attr[i].name);
        if (bi < 0)
            bi = (int)i;
        pack_out(B, bi, pb, &Cb, &Hb, &Wb);
        if (Ha != Hb || Wa != Wb) {
            printf("  shape mismatch %s A %d,%d,%d B %d,%d,%d\n",
                   A->out_attr[i].name, Ca, Ha, Wa, Cb, Hb, Wb);
            mismatch = 1;
            break;
        }
        int vc = Ca < Cb ? Ca : Cb;
        if (compare_nchw(pa, pb, Ca, Ha, Wa, vc, A->out_attr[i].name))
            mismatch = 1;
    }

    double *ta = calloc((size_t)frames, sizeof(double));
    double *tb = calloc((size_t)frames, sizeof(double));
    for (int i = 0; i < frames; i++) {
        double t0 = now_ms();
        write_in(A, ax, img, img_n);
        write_in(A, ar, ref, ref_n);
        if (run_model(A))
            return -1;
        ta[i] = now_ms() - t0;

        t0 = now_ms();
        space_to_depth_nhwc_f16(img, std, H, W, C, bs);
        write_in(B, bx, std, std_n);
        write_in(B, br, ref, B->in_attr[br].n_elems);
        if (run_model(B))
            return -1;
        tb[i] = now_ms() - t0;
    }
    stats(ta, frames, &out->a_ms, &out->a_std);
    stats(tb, frames, &out->b_ms, &out->b_std);
    out->match = !mismatch;
    out->custom_hits = g_custom_hits;
    free(img);
    free(std);
    free(ref);
    free(pa);
    free(pb);
    free(ta);
    free(tb);
    return 0;
}

static int logical_c(const rknn_tensor_attr *a)
{
    /* 输出 attr：NCHW 时 dims[1]=C，NHWC 时 dims[3]=C */
    if (a->fmt == RKNN_TENSOR_NHWC && a->n_dims >= 4)
        return (int)a->dims[3];
    if (a->n_dims >= 4)
        return (int)a->dims[1];
    return 0;
}

static int bench_dec_d2s(model_t *A, model_t *B, int warmup, int frames,
                         bench_result *out)
{
    const char *keys[] = {"z_raw", "y_raw_0", "y_raw_1", "ref_feature"};
    int ai[4], bi[4];
    for (int k = 0; k < 4; k++) {
        ai[k] = find_name(A->in_attr, A->io.n_input, keys[k]);
        bi[k] = find_name(B->in_attr, B->io.n_input, keys[k]);
        if (ai[k] < 0 || bi[k] < 0) {
            fprintf(stderr, "missing input %s\n", keys[k]);
            return -1;
        }
    }
    int ax = find_name(A->out_attr, A->io.n_output, "x_hat");
    int bx = find_name(B->out_attr, B->io.n_output, "x_hat");
    int af = find_name(A->out_attr, A->io.n_output, "feature");
    int bf = find_name(B->out_attr, B->io.n_output, "feature");
    if (ax < 0 || bx < 0 || af < 0 || bf < 0)
        return -1;

    uint16_t *bufs[4];
    for (int k = 0; k < 4; k++) {
        bufs[k] = malloc(A->in_attr[ai[k]].n_elems * 2);
        if (!bufs[k])
            return -1;
        fill_pat(bufs[k], A->in_attr[ai[k]].n_elems);
        write_in(A, ai[k], bufs[k], A->in_attr[ai[k]].n_elems);
        write_in(B, bi[k], bufs[k], B->in_attr[bi[k]].n_elems);
    }

    for (int i = 0; i < warmup; i++) {
        if (run_model(A) || run_model(B))
            return -1;
    }
    if (run_model(A) || run_model(B))
        return -1;

    uint16_t *pa = malloc(16 * 1024 * 1024);
    uint16_t *pb = malloc(16 * 1024 * 1024);
    uint16_t *psh = malloc(16 * 1024 * 1024);
    if (!pa || !pb || !psh)
        return -1;

    int Ca, Ha, Wa, Cb, Hb, Wb;
    pack_out(A, ax, pa, &Ca, &Ha, &Wa);
    pack_out(B, bx, pb, &Cb, &Hb, &Wb);
    int bs = 8;
    int valid_c = logical_c(&A->out_attr[ax]);
    if (valid_c <= 0)
        valid_c = 3;
    printf("  DepthToSpace CPU bs=%d  B %dx%dx%d → expect %dx%dx%d (valid_c=%d)\n",
           bs, Cb, Hb, Wb, valid_c, Hb * bs, Wb * bs, valid_c);
    size_t n_img = (size_t)valid_c * (Hb * bs) * (Wb * bs);
    uint16_t *crd = malloc(n_img * 2);
    uint16_t *dcr = malloc(n_img * 2);
    if (!crd || !dcr)
        return -1;
    depth_to_space_nchw_crd_f16(pb, crd, Cb, Hb, Wb, bs);
    clip01_f16_inplace(crd, n_img);
    depth_to_space_nchw_dcr_f16(pb, dcr, Cb, Hb, Wb, bs);
    clip01_f16_inplace(dcr, n_img);
    int mismatch = 0;
    if (Ha != Hb * bs || Wa != Wb * bs) {
        printf("  spatial mismatch A %d,%d B after d2s %d,%d\n", Ha, Wa, Hb * bs,
               Wb * bs);
        mismatch = 1;
    } else {
        printf("  try CRD:\n");
        int crd_bad = compare_nchw(pa, crd, Ca, Ha, Wa, valid_c, "x_hat_crd");
        printf("  try DCR:\n");
        int dcr_bad = compare_nchw(pa, dcr, Ca, Ha, Wa, valid_c, "x_hat_dcr");
        if (!crd_bad) {
            memcpy(psh, crd, n_img * 2);
        } else if (!dcr_bad) {
            memcpy(psh, dcr, n_img * 2);
        } else {
            mismatch = 1;
            memcpy(psh, crd, n_img * 2);
        }
        (void)dcr_bad;
    }
    free(crd);
    free(dcr);
    int Cfa, Hfa, Wfa, Cfb, Hfb, Wfb;
    pack_out(A, af, pa, &Cfa, &Hfa, &Wfa);
    pack_out(B, bf, pb, &Cfb, &Hfb, &Wfb);
    int vc = Cfa < Cfb ? Cfa : Cfb;
    if (Hfa != Hfb || Wfa != Wfb ||
        compare_nchw(pa, pb, Cfa, Hfa, Wfa, vc, "feature"))
        mismatch = 1;

    double *ta = calloc((size_t)frames, sizeof(double));
    double *tb = calloc((size_t)frames, sizeof(double));
    for (int i = 0; i < frames; i++) {
        double t0 = now_ms();
        for (int k = 0; k < 4; k++)
            write_in(A, ai[k], bufs[k], A->in_attr[ai[k]].n_elems);
        if (run_model(A))
            return -1;
        ta[i] = now_ms() - t0;

        t0 = now_ms();
        for (int k = 0; k < 4; k++)
            write_in(B, bi[k], bufs[k], B->in_attr[bi[k]].n_elems);
        if (run_model(B))
            return -1;
        pack_out(B, bx, pb, &Cb, &Hb, &Wb);
        depth_to_space_nchw_dcr_f16(pb, psh, Cb, Hb, Wb, bs);
        clip01_f16_inplace(psh, (size_t)valid_c * (Hb * bs) * (Wb * bs));
        tb[i] = now_ms() - t0;
    }
    stats(ta, frames, &out->a_ms, &out->a_std);
    stats(tb, frames, &out->b_ms, &out->b_std);
    out->match = !mismatch;
    out->custom_hits = g_custom_hits;
    for (int k = 0; k < 4; k++)
        free(bufs[k]);
    free(pa);
    free(pb);
    free(psh);
    free(ta);
    free(tb);
    return 0;
}

static int bench_custom_std(model_t *A, model_t *B, int warmup, int frames,
                            bench_result *out)
{
    /* A 无 custom，B 有 custom，同一份权重。输入相同。 */
    g_custom_hits = 0;
    int ax = find_name(A->in_attr, A->io.n_input, "x");
    int ar = find_name(A->in_attr, A->io.n_input, "ref_feature");
    int bx = find_name(B->in_attr, B->io.n_input, "x");
    int br = find_name(B->in_attr, B->io.n_input, "ref_feature");
    if (ax < 0 || ar < 0 || bx < 0 || br < 0)
        return -1;
    size_t xn = A->in_attr[ax].n_elems, rn = A->in_attr[ar].n_elems;
    uint16_t *x = malloc(xn * 2), *r = malloc(rn * 2);
    if (!x || !r)
        return -1;
    fill_pat(x, xn);
    memset(r, 0, rn * 2);
    write_in(A, ax, x, xn);
    write_in(A, ar, r, rn);
    write_in(B, bx, x, xn);
    write_in(B, br, r, rn);
    for (int i = 0; i < warmup; i++) {
        if (run_model(A) || run_model(B))
            return -1;
    }
    g_custom_hits = 0;
    if (run_model(A) || run_model(B))
        return -1;
    int mismatch = 0;
    uint16_t *pa = malloc(16 * 1024 * 1024);
    uint16_t *pb = malloc(16 * 1024 * 1024);
    for (uint32_t i = 0; i < A->io.n_output; i++) {
        int Ca, Ha, Wa, Cb, Hb, Wb;
        pack_out(A, (int)i, pa, &Ca, &Ha, &Wa);
        pack_out(B, (int)i, pb, &Cb, &Hb, &Wb);
        int vc = Ca < Cb ? Ca : Cb;
        if (Ha != Hb || Wa != Wb ||
            compare_nchw(pa, pb, Ca, Ha, Wa, vc, A->out_attr[i].name))
            mismatch = 1;
    }
    printf("  custom SpaceToDepth hits after 1 run: %d\n", g_custom_hits);
    double *ta = calloc((size_t)frames, sizeof(double));
    double *tb = calloc((size_t)frames, sizeof(double));
    int hits0 = g_custom_hits;
    for (int i = 0; i < frames; i++) {
        double t0 = now_ms();
        write_in(A, ax, x, xn);
        write_in(A, ar, r, rn);
        if (run_model(A))
            return -1;
        ta[i] = now_ms() - t0;
        t0 = now_ms();
        write_in(B, bx, x, xn);
        write_in(B, br, r, rn);
        if (run_model(B))
            return -1;
        tb[i] = now_ms() - t0;
    }
    stats(ta, frames, &out->a_ms, &out->a_std);
    stats(tb, frames, &out->b_ms, &out->b_std);
    out->match = !mismatch;
    out->custom_hits = g_custom_hits - hits0;
    free(x);
    free(r);
    free(pa);
    free(pb);
    free(ta);
    free(tb);
    return 0;
}

static void print_result(const char *exp, const bench_result *r)
{
    int faster = r->b_ms + 0.5 < r->a_ms; /* 至少快 0.5ms，避免噪声 */
    int pass = r->match && faster;
    printf("\nRESULT exp=%s match=%d a_ms=%.3f±%.3f b_ms=%.3f±%.3f "
           "delta_ms=%.3f custom_hits=%d faster=%d PASS=%d\n",
           exp, r->match, r->a_ms, r->a_std, r->b_ms, r->b_std,
           r->a_ms - r->b_ms, r->custom_hits, faster, pass);
    if (!r->match)
        printf("  判定: 输出不是 1:1，丢弃（即使更快）\n");
    else if (!faster)
        printf("  判定: 1:1 成立，但没有真的变快，丢弃\n");
    else
        printf("  判定: 1:1 且更快，保留\n");
}

int main(int argc, char **argv)
{
    const char *exp = NULL, *path_a = NULL, *path_b = NULL;
    int warmup = 10, frames = 40;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exp") && i + 1 < argc)
            exp = argv[++i];
        else if (!strcmp(argv[i], "--a") && i + 1 < argc)
            path_a = argv[++i];
        else if (!strcmp(argv[i], "--b") && i + 1 < argc)
            path_b = argv[++i];
        else if (!strcmp(argv[i], "--warmup") && i + 1 < argc)
            warmup = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else {
            fprintf(stderr, "unknown arg %s\n", argv[i]);
            return 2;
        }
    }
    if (!exp || !path_a) {
        fprintf(stderr, "usage: rknn_split_bench --exp enc_std|dec_d2s|custom_std "
                        "--a a.rknn [--b b.rknn]\n");
        return 2;
    }
    model_t A, B;
    memset(&A, 0, sizeof(A));
    memset(&B, 0, sizeof(B));
    printf("load A %s\n", path_a);
    if (model_load(&A, path_a, 0))
        return 1;
    bench_result r = {0};
    int rc = 0;
    if (!strcmp(exp, "custom_std")) {
        printf("load B (custom) %s\n", path_b ? path_b : path_a);
        if (model_load(&B, path_b ? path_b : path_a, 1))
            return 1;
        rc = bench_custom_std(&A, &B, warmup, frames, &r);
    } else {
        if (!path_b) {
            fprintf(stderr, "--b required\n");
            return 2;
        }
        printf("load B %s\n", path_b);
        if (model_load(&B, path_b, 0))
            return 1;
        if (!strcmp(exp, "enc_std"))
            rc = bench_enc_std(&A, &B, warmup, frames, &r);
        else if (!strcmp(exp, "dec_d2s"))
            rc = bench_dec_d2s(&A, &B, warmup, frames, &r);
        else {
            fprintf(stderr, "unknown exp %s\n", exp);
            return 2;
        }
    }
    if (rc) {
        fprintf(stderr, "bench failed\n");
        model_free(&A);
        model_free(&B);
        return 1;
    }
    print_result(exp, &r);
    model_free(&A);
    model_free(&B);
    return r.match && (r.b_ms + 0.5 < r.a_ms) ? 0 : 3;
}
