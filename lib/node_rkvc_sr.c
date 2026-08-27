/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_rkvc_sr.c
 * @brief Phase-RLFN RKNN 超分（NV12 + RGA bicubic + phase residual）。
 *
 * 仅支持 Puiching-Memory/rknn-super-resolution 的单输入 fallback core：
 * uint8 phase tensor 12x(H/2)x(W/2) -> 108x(H/2)x(W/2)。宿主字节序不对称：
 * 输入属性为 NHWC（dims 1xHxWx12，按像素通道交错打包），输出属性为
 * NCHW（dims 1x108xHxW，按平面主序读取）——rknn 驱动直接按属性 dims
 * 解释宿主缓冲，不做自动转置。codec-aware
 * 双输入模型和旧 3 通道 RGB 端到端模型均明确拒绝。
 */

#include "internal.h"
#include "platform.h"
#include "rkvc_sr_phase.h"

#ifdef RKVC_ENABLE_RKNN

#include <rknn_api.h>
#include <string.h>

#include "rknn_util.h"

#define RKVC_SR_SLOTS 2
#define RKVC_RKNN_CRYPT_MAGIC     "CYPTRKNN"
#define RKVC_RKNN_CRYPT_MAGIC_LEN  8
#define RKVC_RKNN_CRYPT_HDR_SIZE   16
/** 自研加密层模型 magic（见 lib/model_crypt_layout.h，拒绝检测不依赖选项） */
#define RKVC_MODEL_ENC_MAGIC_STR  "RKVCENC1"

typedef struct rkvc_sr_slot {
    uint8_t     *phase_input;
    size_t       phase_input_size;
    rkvc_buffer *nv12_base;
    int64_t      pts;
} rkvc_sr_slot;

struct rkvc_rknn_sr_ctx {
    rknn_context      ctx;
    int               core_w;
    int               core_h;
    int               frame_in_w;
    int               frame_in_h;
    int               frame_out_w;
    int               frame_out_h;
    rknn_tensor_attr  in_attr;
    rknn_tensor_attr  out_attr;
    rkvc_sr_slot      slots[RKVC_SR_SLOTS];
    int               run_slot;
    int               busy;
    rkvc_buffer      *resize_buf;
    rkvc_buffer_pool *pool;
};

static int rknn_model_crypt_level(const void *model, size_t size)
{
    if (!model || size < RKVC_RKNN_CRYPT_HDR_SIZE)
        return 0;
    const uint8_t *p = model;
    if (memcmp(p, RKVC_RKNN_CRYPT_MAGIC, RKVC_RKNN_CRYPT_MAGIC_LEN) != 0)
        return 0;
    /* rknn-toolkit2 2.3.x 加密格式：+0 magic，+8 version(u64)，
     * +0x10 crypt_level(u32)，+0x18 明文长度。旧代码误读 +8 的
     * version 字段（恰为 1 而碰巧可用），此处改为正确偏移。 */
    if (size < 20)
        return 0;
    const uint32_t level = (uint32_t)p[16] | ((uint32_t)p[17] << 8) |
                           ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);
    return level >= 1 && level <= 3 ? (int)level : 0;
}

static rkvc_err rknn_load_model(const char *model_path, rknn_context *out)
{
    void *model;
    size_t fsize;
#ifdef RKVC_ENABLE_MODEL_CRYPT
    /* 自研加密层：加密模型需每机 model.key 解密，明文模型原样透传 */
    const rkvc_err ferr = rkvc_model_crypt_load_file(model_path, &model,
                                                     &fsize);
    if (ferr != RKVC_OK)
        return ferr;
#else
    FILE *fp = fopen(model_path, "rb");
    if (!fp)
        return RKVC_ERR_NOT_FOUND;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    const long sz = ftell(fp);
    if (sz <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    model = rkvc_malloc((size_t)sz);
    if (!model) {
        fclose(fp);
        return RKVC_ERR_NOMEM;
    }
    if (fread(model, 1, (size_t)sz, fp) != (size_t)sz) {
        rkvc_free(model);
        fclose(fp);
        return RKVC_ERR_IO;
    }
    fclose(fp);
    fsize = (size_t)sz;
    if (fsize >= 8 && memcmp(model, RKVC_MODEL_ENC_MAGIC_STR, 8) == 0) {
        rkvc_free(model);
        RKVC_LOG("rknn sr model %s is rkvc-encrypted but built "
                 "without RKVC_ENABLE_MODEL_CRYPT", model_path);
        return RKVC_ERR_FORMAT;
    }
#endif

    const int crypt_level = rknn_model_crypt_level(model, fsize);
    char sha256[65];
    if (rkvc_hash_buffer("sha256", model, fsize,
                         sha256, sizeof(sha256)) == RKVC_OK)
        RKVC_LOG("rknn sr model sha256=%s path=%s", sha256, model_path);
    rkvc_rknn_quiet_runtime();
    const int ret = rknn_init(out, model, (uint32_t)fsize, 0, NULL);
    rkvc_secure_zero_free(model, fsize);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("rknn_init failed: %d (crypt_level=%d)", ret, crypt_level);
        return RKVC_ERR_HW;
    }
    return RKVC_OK;
}

static int tensor_chw(const rknn_tensor_attr *attr, int *c, int *h, int *w)
{
    if (!attr || attr->n_dims != 4 || !c || !h || !w)
        return 0;
    if (attr->fmt == RKNN_TENSOR_NCHW || attr->fmt == RKNN_TENSOR_UNDEFINED) {
        *c = (int)attr->dims[1];
        *h = (int)attr->dims[2];
        *w = (int)attr->dims[3];
    } else if (attr->fmt == RKNN_TENSOR_NHWC) {
        /* rknn-toolkit2 把 NCHW ONNX 图的宿主输入属性记为 NHWC
         * （dims 1xHxWxC）；驱动直接按该 dims 解释宿主缓冲，因此
         * 输入必须按 NHWC 交错打包，输出属性若为 NCHW 则按平面读取。 */
        *h = (int)attr->dims[1];
        *w = (int)attr->dims[2];
        *c = (int)attr->dims[3];
    } else {
        return 0;
    }
    return *c > 0 && *h > 0 && *w > 0;
}

static rkvc_err sr_slot_alloc(rkvc_rknn_sr_ctx *ctx, rkvc_sr_slot *slot)
{
    slot->phase_input_size = (size_t)ctx->in_attr.n_elems;
    slot->phase_input = rkvc_malloc(slot->phase_input_size);
    if (!slot->phase_input)
        return RKVC_ERR_NOMEM;
    rkvc_err err = rkvc_buffer_pool_alloc_video_cached(
        ctx->pool, &slot->nv12_base, ctx->frame_out_w, ctx->frame_out_h,
        RKVC_PIX_FMT_NV12);
    return err;
}

static void sr_slot_free(rkvc_sr_slot *slot)
{
    if (!slot)
        return;
    rkvc_free(slot->phase_input);
    rkvc_buffer_unref(slot->nv12_base);
    memset(slot, 0, sizeof(*slot));
}

static rkvc_err sr_slot_ensure_writable_base(rkvc_rknn_sr_ctx *ctx,
                                             rkvc_sr_slot *slot)
{
    int exclusively_owned = 0;
    if (slot->nv12_base) {
        pthread_mutex_lock(&slot->nv12_base->lock);
        exclusively_owned = slot->nv12_base->ref_count == 1;
        pthread_mutex_unlock(&slot->nv12_base->lock);
    }
    if (exclusively_owned)
        return RKVC_OK;

    rkvc_buffer_unref(slot->nv12_base);
    slot->nv12_base = NULL;
    return rkvc_buffer_pool_alloc_video_cached(
        ctx->pool, &slot->nv12_base, ctx->frame_out_w, ctx->frame_out_h,
        RKVC_PIX_FMT_NV12);
}

static rkvc_err sr_resize_input(rkvc_rknn_sr_ctx *ctx,
                                const rkvc_buffer *src,
                                const rkvc_buffer **feed)
{
    *feed = src;
    if (src->av_frame->width == ctx->frame_in_w &&
        src->av_frame->height == ctx->frame_in_h)
        return RKVC_OK;

    rkvc_err err;
    if (!ctx->resize_buf) {
        err = rkvc_rga_scale_buffer_cached(src, &ctx->resize_buf,
                                           ctx->frame_in_w, ctx->frame_in_h,
                                           RKVC_PIX_FMT_NV12,
                                           RKVC_UPSCALE_BILINEAR);
    } else {
        err = rkvc_rga_scale_buffer_into(src, ctx->resize_buf,
                                         RKVC_UPSCALE_BILINEAR);
    }
    if (err != RKVC_OK)
        return err;
    *feed = ctx->resize_buf;
    return RKVC_OK;
}

static rkvc_err sr_prepare_input(rkvc_rknn_sr_ctx *ctx, rkvc_sr_slot *slot,
                                 const rkvc_buffer *src)
{
    if (!src->av_frame || src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;
    const rkvc_buffer *feed = NULL;
    rkvc_err err = sr_resize_input(ctx, src, &feed);
    if (err != RKVC_OK)
        return err;
    err = rkvc_buffer_dmabuf_begin_cpu_read(feed);
    if (err != RKVC_OK)
        return err;
    const int pack_rc = rkvc_sr_phase_pack_nv12(feed->av_frame->data[0],
                                feed->av_frame->linesize[0],
                                feed->av_frame->data[1],
                                feed->av_frame->linesize[1],
                                ctx->frame_in_w, ctx->frame_in_h,
                                slot->phase_input, slot->phase_input_size);
    const rkvc_err sync_err = rkvc_buffer_dmabuf_end_cpu_read(feed);
    if (pack_rc != 0)
        return RKVC_ERR_FORMAT;
    if (sync_err != RKVC_OK)
        return sync_err;

    /* 正常流水线复用双 slot；下游仍持有旧输出时分配替代缓冲，避免覆写。 */
    err = sr_slot_ensure_writable_base(ctx, slot);
    if (err != RKVC_OK)
        return err;
    err = rkvc_rga_scale_buffer_into(feed, slot->nv12_base,
                                     RKVC_UPSCALE_BICUBIC);
    if (err != RKVC_OK)
        return err;
    slot->pts = src->pts;
    return RKVC_OK;
}

static rkvc_err sr_postprocess_output(rkvc_rknn_sr_ctx *ctx,
                                      rkvc_sr_slot *slot,
                                      rkvc_buffer **out)
{
    rknn_output output;
    memset(&output, 0, sizeof(output));
    output.index = 0;
    output.want_float = 1;
    if (rknn_outputs_get(ctx->ctx, 1, &output, NULL) != RKNN_SUCC)
        return RKVC_ERR_HW;

    const size_t required = (size_t)ctx->out_attr.n_elems * sizeof(float);
    rkvc_err err = RKVC_OK;
    if (!output.buf || output.size < required || !slot->nv12_base) {
        err = RKVC_ERR_HW;
    } else {
        err = rkvc_buffer_dmabuf_begin_cpu_rw(slot->nv12_base);
        if (err == RKVC_OK) {
            if (rkvc_sr_phase_add_residual_nv12(
                output.buf, ctx->core_w, ctx->core_h,
                slot->nv12_base->av_frame->data[0],
                slot->nv12_base->av_frame->linesize[0],
                slot->nv12_base->av_frame->data[1],
                slot->nv12_base->av_frame->linesize[1],
                ctx->frame_out_w, ctx->frame_out_h) != 0)
                err = RKVC_ERR_HW;
            const rkvc_err end_err =
                rkvc_buffer_dmabuf_end_cpu_rw(slot->nv12_base);
            if (err == RKVC_OK)
                err = end_err;
        }
    }
    rknn_outputs_release(ctx->ctx, 1, &output);
    if (err != RKVC_OK)
        return err;
    slot->nv12_base->pts = slot->pts;
    *out = rkvc_buffer_ref(slot->nv12_base);
    return RKVC_OK;
}

static void sr_discard_output(rkvc_rknn_sr_ctx *ctx)
{
    if (!ctx || !ctx->busy)
        return;
    rknn_output output;
    memset(&output, 0, sizeof(output));
    output.index = 0;
    if (rknn_outputs_get(ctx->ctx, 1, &output, NULL) == RKNN_SUCC)
        rknn_outputs_release(ctx->ctx, 1, &output);
    ctx->busy = 0;
}

int rkvc_rknn_sr_available(void)
{
    const rkvc_platform_info *pi = rkvc_platform_probe();
    return pi->has_npu && rkvc_npu_accessible() && rkvc_rga_available();
}

rkvc_rknn_sr_ctx *rkvc_rknn_sr_ctx_create(const char *model_path,
                                          int expect_out_w, int expect_out_h,
                                          rkvc_buffer_pool *pool)
{
    if (!model_path || !model_path[0] || expect_out_w <= 0 || expect_out_h <= 0 ||
        !rkvc_rga_available())
        return NULL;
    rkvc_rknn_sr_ctx *ctx = rkvc_calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->run_slot = -1;
    ctx->pool = pool;
    if (rknn_load_model(model_path, &ctx->ctx) != RKVC_OK)
        goto fail;

    rknn_sdk_version ver;
    memset(&ver, 0, sizeof(ver));
    if (rknn_query(ctx->ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC)
        RKVC_LOG("rknnrt api=%s drv=%s", ver.api_version, ver.drv_version);
    const rkvc_platform_info *pi = rkvc_platform_probe();
    if (pi->npu_cores > 1)
        rkvc_rknn_apply_npu_cores(ctx->ctx, pi->npu_cores);

    rknn_input_output_num io_num;
    if (rknn_query(ctx->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) !=
        RKNN_SUCC)
        goto fail;
    if (io_num.n_input != 1 || io_num.n_output != 1) {
        RKVC_LOG("Phase-RLFN requires one input/output, got %u/%u; "
                 "export with --no-codec-context", io_num.n_input, io_num.n_output);
        goto fail;
    }

    ctx->in_attr.index = 0;
    ctx->out_attr.index = 0;
    if (rknn_query(ctx->ctx, RKNN_QUERY_INPUT_ATTR, &ctx->in_attr,
                   sizeof(ctx->in_attr)) != RKNN_SUCC ||
        rknn_query(ctx->ctx, RKNN_QUERY_OUTPUT_ATTR, &ctx->out_attr,
                   sizeof(ctx->out_attr)) != RKNN_SUCC)
        goto fail;

    int in_c = 0, in_h = 0, in_w = 0;
    int out_c = 0, out_h = 0, out_w = 0;
    if (!tensor_chw(&ctx->in_attr, &in_c, &in_h, &in_w) ||
        !tensor_chw(&ctx->out_attr, &out_c, &out_h, &out_w) ||
        in_c != RKVC_SR_PHASE_INPUT_CHANNELS ||
        out_c != RKVC_SR_PHASE_OUTPUT_CHANNELS ||
        in_w != out_w || in_h != out_h) {
        RKVC_LOG("unsupported SR contract: in=%dx%dx%d fmt=%d "
                 "out=%dx%dx%d fmt=%d (required 12->108)",
                 in_c, in_h, in_w, (int)ctx->in_attr.fmt,
                 out_c, out_h, out_w, (int)ctx->out_attr.fmt);
        goto fail;
    }

    ctx->core_w = in_w;
    ctx->core_h = in_h;
    ctx->frame_in_w = in_w * RKVC_SR_PHASE_INPUT_FACTOR;
    ctx->frame_in_h = in_h * RKVC_SR_PHASE_INPUT_FACTOR;
    ctx->frame_out_w = out_w * RKVC_SR_PHASE_OUTPUT_FACTOR;
    ctx->frame_out_h = out_h * RKVC_SR_PHASE_OUTPUT_FACTOR;
    if (ctx->frame_out_w != expect_out_w || ctx->frame_out_h != expect_out_h) {
        RKVC_LOG("Phase-RLFN output %dx%d != expected %dx%d",
                 ctx->frame_out_w, ctx->frame_out_h,
                 expect_out_w, expect_out_h);
        goto fail;
    }
    for (int i = 0; i < RKVC_SR_SLOTS; i++) {
        if (sr_slot_alloc(ctx, &ctx->slots[i]) != RKVC_OK)
            goto fail;
    }
    RKVC_LOG("Phase-RLFN SR %s: frame %dx%d -> %dx%d "
             "(core 12x%dx%d -> 108x%dx%d)",
             model_path, ctx->frame_in_w, ctx->frame_in_h,
             ctx->frame_out_w, ctx->frame_out_h,
             ctx->core_h, ctx->core_w, ctx->core_h, ctx->core_w);
    return ctx;

fail:
    rkvc_rknn_sr_ctx_destroy(ctx);
    return NULL;
}

void rkvc_rknn_sr_ctx_destroy(rkvc_rknn_sr_ctx *ctx)
{
    if (!ctx)
        return;
    sr_discard_output(ctx);
    for (int i = 0; i < RKVC_SR_SLOTS; i++)
        sr_slot_free(&ctx->slots[i]);
    rkvc_buffer_unref(ctx->resize_buf);
    if (ctx->ctx)
        rknn_destroy(ctx->ctx);
    rkvc_free(ctx);
}

int rkvc_rknn_sr_ctx_busy(const rkvc_rknn_sr_ctx *ctx)
{
    return ctx && ctx->busy;
}

void rkvc_rknn_sr_ctx_drain(rkvc_rknn_sr_ctx *ctx)
{
    sr_discard_output(ctx);
}

rkvc_err rkvc_rknn_sr_ctx_submit(rkvc_rknn_sr_ctx *ctx,
                                 const rkvc_buffer *src)
{
    if (!ctx || !src || !src->av_frame)
        return RKVC_ERR_INVALID;
    if (src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;
    if (ctx->busy)
        return RKVC_ERR_AGAIN;

    const int slot_idx = ctx->run_slot < 0 ? 0 : (ctx->run_slot ^ 1);
    rkvc_sr_slot *slot = &ctx->slots[slot_idx];
    rkvc_err err = sr_prepare_input(ctx, slot, src);
    if (err != RKVC_OK)
        return err;

    rknn_input input;
    memset(&input, 0, sizeof(input));
    input.index = 0;
    input.buf = slot->phase_input;
    input.size = (uint32_t)slot->phase_input_size;
    input.type = RKNN_TENSOR_UINT8;
    /* 声明与模型属性一致的 fmt：rknn-toolkit2 把 NCHW ONNX 图的宿主
     * 输入属性记为 NHWC（dims 1xHxWxC），且驱动直接按该 dims 解释宿主
     * 缓冲字节序（不自动转置，恒等模型实测）；phase 打包因此按 NHWC
     * 交错，而输出属性仍是 NCHW，残差按平面主序读取。 */
    input.fmt = ctx->in_attr.fmt == RKNN_TENSOR_NHWC ? RKNN_TENSOR_NHWC
                                                     : RKNN_TENSOR_NCHW;
    int ret = rknn_inputs_set(ctx->ctx, 1, &input);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("Phase-RLFN input set failed: %d", ret);
        return RKVC_ERR_HW;
    }
    ret = rknn_run(ctx->ctx, NULL);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("Phase-RLFN rknn_run failed: %d", ret);
        return RKVC_ERR_HW;
    }
    ctx->run_slot = slot_idx;
    ctx->busy = 1;
    return RKVC_OK;
}

rkvc_err rkvc_rknn_sr_ctx_collect(rkvc_rknn_sr_ctx *ctx,
                                  rkvc_buffer **out, int block)
{
    (void)block;
    if (!ctx || !out)
        return RKVC_ERR_INVALID;
    *out = NULL;
    if (!ctx->busy)
        return RKVC_ERR_AGAIN;
    ctx->busy = 0;
    return sr_postprocess_output(ctx, &ctx->slots[ctx->run_slot], out);
}

rkvc_err rkvc_rknn_sr_ctx_process(rkvc_rknn_sr_ctx *ctx,
                                  const rkvc_buffer *src,
                                  rkvc_buffer **out)
{
    rkvc_err err = rkvc_rknn_sr_ctx_submit(ctx, src);
    if (err != RKVC_OK)
        return err;
    return rkvc_rknn_sr_ctx_collect(ctx, out, 1);
}

rkvc_err rkvc_rknn_sr_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                             int dst_w, int dst_h, const char *model_path)
{
    if (!src || !dst || dst_w <= 0 || dst_h <= 0 ||
        !model_path || !model_path[0])
        return RKVC_ERR_INVALID;
    rkvc_rknn_sr_ctx *ctx = rkvc_rknn_sr_ctx_create(model_path, dst_w, dst_h,
                                                    NULL);
    if (!ctx)
        return RKVC_ERR_HW;
    rkvc_err err = rkvc_rknn_sr_ctx_process(ctx, src, dst);
    rkvc_rknn_sr_ctx_destroy(ctx);
    return err;
}

#else

int rkvc_rknn_sr_available(void) { return 0; }
rkvc_rknn_sr_ctx *rkvc_rknn_sr_ctx_create(const char *p, int w, int h,
                                          rkvc_buffer_pool *pool)
{ (void)p; (void)w; (void)h; (void)pool; return NULL; }
void rkvc_rknn_sr_ctx_destroy(rkvc_rknn_sr_ctx *ctx) { (void)ctx; }
int rkvc_rknn_sr_ctx_busy(const rkvc_rknn_sr_ctx *ctx) { (void)ctx; return 0; }
void rkvc_rknn_sr_ctx_drain(rkvc_rknn_sr_ctx *ctx) { (void)ctx; }
rkvc_err rkvc_rknn_sr_ctx_submit(rkvc_rknn_sr_ctx *ctx, const rkvc_buffer *src)
{ (void)ctx; (void)src; return RKVC_ERR_HW; }
rkvc_err rkvc_rknn_sr_ctx_collect(rkvc_rknn_sr_ctx *ctx, rkvc_buffer **out, int block)
{ (void)ctx; (void)out; (void)block; return RKVC_ERR_HW; }
rkvc_err rkvc_rknn_sr_ctx_process(rkvc_rknn_sr_ctx *ctx, const rkvc_buffer *src,
                                  rkvc_buffer **out)
{ (void)ctx; (void)src; (void)out; return RKVC_ERR_HW; }
rkvc_err rkvc_rknn_sr_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                             int w, int h, const char *p)
{ (void)src; (void)dst; (void)w; (void)h; (void)p; return RKVC_ERR_HW; }

#endif
