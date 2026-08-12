/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_rkvc_sr.c
 * @brief RKNN NPU 超分辨率（明文/加密模型、RGA CSC + NEON + 双缓冲异步推理）。
 */

#include "internal.h"
#include "rkvc_sr_neon.h"

#ifdef RKVC_ENABLE_RKNN

#include <rknn_api.h>
#include <string.h>

#define RKVC_SR_SLOTS 2

/** RKNN-Toolkit2 export_encrypted_rknn_model 文件头；crypt_level 0 表示明文 .rknn */
#define RKVC_RKNN_CRYPT_MAGIC     "CYPTRKNN"
#define RKVC_RKNN_CRYPT_MAGIC_LEN  8
#define RKVC_RKNN_CRYPT_HDR_SIZE   16

typedef struct rkvc_sr_slot {
    rknn_tensor_mem *input_mem;
    rknn_tensor_mem *output_mem;
    uint8_t         *rgb_in;
    int              rgb_in_stride;
    uint8_t         *rgb_out;
    int              rgb_out_stride;
    rkvc_buffer     *nv12_out;
    int64_t          pts;
    uint64_t         frame_id;
} rkvc_sr_slot;

struct rkvc_rknn_sr_ctx {
    rknn_context      ctx;
    rkvc_buffer_pool *pool;
    int               in_w;
    int               in_h;
    int               out_w;
    int               out_h;
    rknn_tensor_attr  in_attr;
    rknn_tensor_attr  out_attr;
    rknn_tensor_attr  in_attr_native;
    rknn_tensor_attr  out_attr_native;
    rkvc_sr_slot      slots[RKVC_SR_SLOTS];
    int               run_slot;
    int               busy;
    rkvc_buffer      *resize_buf;
};

static int rknn_model_crypt_level(const void *model, size_t size)
{
    if (!model || size < RKVC_RKNN_CRYPT_HDR_SIZE)
        return 0;
    const uint8_t *p = (const uint8_t *)model;
    if (memcmp(p, RKVC_RKNN_CRYPT_MAGIC, RKVC_RKNN_CRYPT_MAGIC_LEN) != 0)
        return 0;
    const uint32_t level = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                           ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    if (level < 1 || level > 3)
        return 0;
    return (int)level;
}

static rkvc_err rknn_load_model(const char *model_path, rknn_context *out)
{
    FILE *fp = fopen(model_path, "rb");
    if (!fp)
        return RKVC_ERR_NOT_FOUND;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    const long fsize = ftell(fp);
    if (fsize <= 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }

    void *model = rkvc_malloc((size_t)fsize);
    if (!model) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    if (fread(model, 1, (size_t)fsize, fp) != (size_t)fsize) {
        rkvc_free(model);
        fclose(fp);
        return RKVC_ERR_IO;
    }
    fclose(fp);

    const int crypt_level = rknn_model_crypt_level(model, (size_t)fsize);
    RKVC_LOG("rknn model crypt_level=%d", crypt_level);

    {
        char sha256[65];
        if (rkvc_hash_buffer("sha256", (const uint8_t *)model, (size_t)fsize,
                             sha256, sizeof(sha256)) == RKVC_OK) {
            RKVC_LOG("rknn model sha256=%s path=%s", sha256, model_path);
        }
    }

    /* 分发构建：在 rknn_init 前强制 librknnrt 静默，避免 RKNN_LOG_LEVEL
     * 泄露已解密模型的网络结构（见 rkvc_rknn_quiet_runtime 说明）。 */
    rkvc_rknn_quiet_runtime();
    const int ret = rknn_init(out, model, (uint32_t)fsize, 0, NULL);
    rkvc_free(model);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("rknn_init failed: %d (crypt_level=%d, check NPU driver / rknnrt)",
                 ret, crypt_level);
        return RKVC_ERR_HW;
    }
    return RKVC_OK;
}

static int rknn_dims_wh(const rknn_tensor_attr *attr, int *w, int *h)
{
    if (!attr || attr->n_dims < 4 || !w || !h)
        return 0;
    if (attr->fmt == RKNN_TENSOR_NCHW || attr->fmt == RKNN_TENSOR_UNDEFINED) {
        *h = (int)attr->dims[2];
        *w = (int)attr->dims[3];
        return (*w > 0 && *h > 0);
    }
    if (attr->fmt == RKNN_TENSOR_NHWC) {
        *h = (int)attr->dims[1];
        *w = (int)attr->dims[2];
        return (*w > 0 && *h > 0);
    }
    return 0;
}

static rkvc_err sr_slot_alloc_io(rkvc_rknn_sr_ctx *ctx, rkvc_sr_slot *slot)
{
    slot->input_mem = rknn_create_mem2(ctx->ctx,
                                       ctx->in_attr_native.size_with_stride,
                                       RKNN_FLAG_MEMORY_CACHEABLE);
    slot->output_mem = rknn_create_mem2(ctx->ctx,
                                        ctx->out_attr_native.size_with_stride,
                                        RKNN_FLAG_MEMORY_CACHEABLE);
    if (!slot->input_mem || !slot->output_mem)
        return RKVC_ERR_NOMEM;

    slot->rgb_in_stride = rkvc_rga_rgb888_stride(ctx->in_w);
    slot->rgb_out_stride = rkvc_rga_rgb888_stride(ctx->out_w);

    const size_t in_rgb = (size_t)rkvc_rga_rgb888_row_bytes(ctx->in_w) *
                          (size_t)ctx->in_h;
    const size_t out_rgb = (size_t)rkvc_rga_rgb888_row_bytes(ctx->out_w) *
                           (size_t)ctx->out_h;

    slot->rgb_in = rkvc_malloc(in_rgb);
    slot->rgb_out = rkvc_malloc(out_rgb);
    if (!slot->rgb_in || !slot->rgb_out)
        return RKVC_ERR_NOMEM;

    rkvc_err err = rkvc_buffer_pool_alloc_video(ctx->pool, &slot->nv12_out,
                                                ctx->out_w, ctx->out_h,
                                                RKVC_PIX_FMT_NV12,
                                                RKVC_MEM_DMABUF);
    if (err != RKVC_OK) {
        err = rkvc_buffer_alloc_video_host(&slot->nv12_out,
                                           ctx->out_w, ctx->out_h,
                                           RKVC_PIX_FMT_NV12);
        if (err != RKVC_OK)
            return err;
    }

    return RKVC_OK;
}

static void sr_slot_free_io(rkvc_rknn_sr_ctx *ctx, rkvc_sr_slot *slot)
{
    if (!slot)
        return;
    if (ctx && ctx->ctx) {
        if (slot->input_mem) {
            rknn_destroy_mem(ctx->ctx, slot->input_mem);
            slot->input_mem = NULL;
        }
        if (slot->output_mem) {
            rknn_destroy_mem(ctx->ctx, slot->output_mem);
            slot->output_mem = NULL;
        }
    }
    rkvc_buffer_unref(slot->nv12_out);
    slot->nv12_out = NULL;
    rkvc_free(slot->rgb_in);
    slot->rgb_in = NULL;
    rkvc_free(slot->rgb_out);
    slot->rgb_out = NULL;
}

static rkvc_err sr_bind_slot_io(rkvc_rknn_sr_ctx *ctx, int slot_idx)
{
    rkvc_sr_slot *slot = &ctx->slots[slot_idx];

    ctx->in_attr_native.pass_through = 1;
    if (rknn_set_io_mem(ctx->ctx, slot->input_mem,
                        &ctx->in_attr_native) != RKNN_SUCC)
        return RKVC_ERR_HW;
    if (rknn_set_io_mem(ctx->ctx, slot->output_mem,
                        &ctx->out_attr_native) != RKNN_SUCC)
        return RKVC_ERR_HW;
    return RKVC_OK;
}

static rkvc_err sr_ensure_resize_buf(rkvc_rknn_sr_ctx *ctx, int w, int h)
{
    if (ctx->resize_buf &&
        (int)ctx->resize_buf->width == w &&
        (int)ctx->resize_buf->height == h)
        return RKVC_OK;

    rkvc_buffer_unref(ctx->resize_buf);
    ctx->resize_buf = NULL;

    rkvc_err err = rkvc_buffer_pool_alloc_video(ctx->pool, &ctx->resize_buf,
                                                w, h, RKVC_PIX_FMT_NV12,
                                                RKVC_MEM_DMABUF);
    if (err != RKVC_OK) {
        err = rkvc_buffer_alloc_video_host(&ctx->resize_buf, w, h,
                                           RKVC_PIX_FMT_NV12);
    }
    return err;
}

static rkvc_err sr_prepare_input(rkvc_rknn_sr_ctx *ctx, rkvc_sr_slot *slot,
                                 const rkvc_buffer *src)
{
    if (!src->av_frame || src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;

    const int sw = src->av_frame->width;
    const int sh = src->av_frame->height;
    const rkvc_buffer *feed = src;

    if (sw != ctx->in_w || sh != ctx->in_h) {
        rkvc_err err = sr_ensure_resize_buf(ctx, ctx->in_w, ctx->in_h);
        if (err != RKVC_OK)
            return err;
        err = rkvc_rga_scale_buffer(src, &ctx->resize_buf,
                                      ctx->in_w, ctx->in_h,
                                      RKVC_PIX_FMT_NV12,
                                      RKVC_UPSCALE_BILINEAR);
        if (err != RKVC_OK)
            return err;
        feed = ctx->resize_buf;
    }

    rkvc_err err;
    if (rkvc_rga_available()) {
        err = rkvc_rga_csc_nv12_to_rgb888(feed, slot->rgb_in,
                                          ctx->in_w, ctx->in_h,
                                          slot->rgb_in_stride);
    } else {
        struct SwsContext *sws = sws_getContext(ctx->in_w, ctx->in_h,
                                               AV_PIX_FMT_NV12,
                                               ctx->in_w, ctx->in_h,
                                               AV_PIX_FMT_RGB24,
                                               SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws)
            return RKVC_ERR_NOMEM;
        uint8_t *dst_data[1] = { slot->rgb_in };
        int dst_linesize[1] = { rkvc_rga_rgb888_row_bytes(ctx->in_w) };
        sws_scale(sws,
                  (const uint8_t *const *)feed->av_frame->data,
                  feed->av_frame->linesize,
                  0, ctx->in_h, dst_data, dst_linesize);
        sws_freeContext(sws);
        err = RKVC_OK;
    }
    if (err != RKVC_OK)
        return err;

    const size_t row_bytes = (size_t)ctx->in_w * 3;
    int8_t *out_base = (int8_t *)slot->input_mem->virt_addr;
    for (int y = 0; y < ctx->in_h; y++) {
        rkvc_sr_quant_rgb24_nhwc_to_int8(
            slot->rgb_in + y * rkvc_rga_rgb888_row_bytes(ctx->in_w),
            out_base + y * row_bytes,
            row_bytes,
            ctx->in_attr.zp, ctx->in_attr.scale);
    }
    slot->pts = src->pts;
    return RKVC_OK;
}

static rkvc_err sr_postprocess_output(rkvc_rknn_sr_ctx *ctx,
                                        rkvc_sr_slot *slot,
                                        rkvc_buffer **out)
{
    rkvc_sr_dequant_nchw_int8_to_rgb24(
        (const int8_t *)slot->output_mem->virt_addr,
        ctx->out_w, ctx->out_h,
        ctx->out_attr.zp, ctx->out_attr.scale,
        slot->rgb_out, rkvc_rga_rgb888_row_bytes(ctx->out_w));

    rkvc_err err;
    if (rkvc_rga_available()) {
        err = rkvc_rga_csc_rgb888_to_nv12(slot->rgb_out,
                                          ctx->out_w, ctx->out_h,
                                          slot->rgb_out_stride,
                                          slot->nv12_out);
    } else {
        struct SwsContext *sws = sws_getContext(ctx->out_w, ctx->out_h,
                                                AV_PIX_FMT_RGB24,
                                                ctx->out_w, ctx->out_h,
                                                AV_PIX_FMT_NV12,
                                                SWS_BILINEAR, NULL, NULL, NULL);
        if (!sws)
            return RKVC_ERR_NOMEM;
        const uint8_t *src_rgb[1] = { slot->rgb_out };
        int src_stride[1] = { rkvc_rga_rgb888_row_bytes(ctx->out_w) };
        sws_scale(sws, src_rgb, src_stride, 0, ctx->out_h,
                  slot->nv12_out->av_frame->data,
                  slot->nv12_out->av_frame->linesize);
        sws_freeContext(sws);
        err = RKVC_OK;
    }
    if (err != RKVC_OK) {
        RKVC_LOG("rknn sr postprocess csc failed: %d", (int)err);
        return err;
    }

    slot->nv12_out->pts = slot->pts;
    *out = rkvc_buffer_ref(slot->nv12_out);
    return RKVC_OK;
}

static rkvc_err sr_wait_slot(rkvc_rknn_sr_ctx *ctx, int block)
{
    (void)block;
    rkvc_sr_slot *slot = &ctx->slots[ctx->run_slot];

    if (rknn_mem_sync(ctx->ctx, slot->output_mem,
                      RKNN_MEMORY_SYNC_FROM_DEVICE) != RKNN_SUCC) {
        RKVC_LOG("rknn mem sync from device failed");
        return RKVC_ERR_HW;
    }

    ctx->busy = 0;
    return RKVC_OK;
}

int rkvc_rknn_sr_available(void)
{
    return 1;
}

rkvc_rknn_sr_ctx *rkvc_rknn_sr_ctx_create(const char *model_path,
                                          int expect_out_w, int expect_out_h,
                                          rkvc_buffer_pool *pool)
{
    if (!model_path || !model_path[0] || expect_out_w <= 0 || expect_out_h <= 0)
        return NULL;

    rkvc_rknn_sr_ctx *ctx = rkvc_calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->pool = pool;
    ctx->run_slot = -1;

    if (rknn_load_model(model_path, &ctx->ctx) != RKVC_OK)
        goto fail;

    {
        rknn_sdk_version ver;
        memset(&ver, 0, sizeof(ver));
        if (rknn_query(ctx->ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) ==
            RKNN_SUCC) {
            RKVC_LOG("rknnrt api=%s drv=%s", ver.api_version, ver.drv_version);
        } else {
            RKVC_LOG("rknn_query SDK_VERSION failed");
        }
    }

    rknn_set_core_mask(ctx->ctx, RKNN_NPU_CORE_0_1_2);

    rknn_input_output_num io_num;
    if (rknn_query(ctx->ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) !=
        RKNN_SUCC)
        goto fail;

    ctx->in_attr.index = 0;
    if (rknn_query(ctx->ctx, RKNN_QUERY_INPUT_ATTR, &ctx->in_attr,
                   sizeof(ctx->in_attr)) != RKNN_SUCC)
        goto fail;
    ctx->out_attr.index = 0;
    if (rknn_query(ctx->ctx, RKNN_QUERY_OUTPUT_ATTR, &ctx->out_attr,
                   sizeof(ctx->out_attr)) != RKNN_SUCC)
        goto fail;

    ctx->in_attr_native.index = 0;
    if (rknn_query(ctx->ctx, RKNN_QUERY_NATIVE_INPUT_ATTR,
                   &ctx->in_attr_native,
                   sizeof(ctx->in_attr_native)) != RKNN_SUCC)
        goto fail;
    ctx->out_attr_native.index = 0;
    if (rknn_query(ctx->ctx, RKNN_QUERY_NATIVE_OUTPUT_ATTR,
                   &ctx->out_attr_native,
                   sizeof(ctx->out_attr_native)) != RKNN_SUCC)
        goto fail;

    if (!rknn_dims_wh(&ctx->in_attr, &ctx->in_w, &ctx->in_h) ||
        !rknn_dims_wh(&ctx->out_attr, &ctx->out_w, &ctx->out_h))
        goto fail;

    if (ctx->out_w != expect_out_w || ctx->out_h != expect_out_h) {
        RKVC_LOG("rknn model output %dx%d != expected %dx%d",
                 ctx->out_w, ctx->out_h, expect_out_w, expect_out_h);
        goto fail;
    }

    for (int i = 0; i < RKVC_SR_SLOTS; i++) {
        if (sr_slot_alloc_io(ctx, &ctx->slots[i]) != RKVC_OK)
            goto fail;
    }

    RKVC_LOG("rknn sr %s: in %dx%d -> out %dx%d (slots=%d rga=%d)",
             model_path, ctx->in_w, ctx->in_h, ctx->out_w, ctx->out_h,
             RKVC_SR_SLOTS, rkvc_rga_available());
    return ctx;

fail:
    rkvc_rknn_sr_ctx_destroy(ctx);
    return NULL;
}

void rkvc_rknn_sr_ctx_destroy(rkvc_rknn_sr_ctx *ctx)
{
    if (!ctx)
        return;

    if (ctx->busy)
        sr_wait_slot(ctx, 1);

    for (int i = 0; i < RKVC_SR_SLOTS; i++)
        sr_slot_free_io(ctx, &ctx->slots[i]);

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
    if (!ctx || !ctx->busy)
        return;
    sr_wait_slot(ctx, 1);
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
    if (err != RKVC_OK) {
        RKVC_LOG("rknn sr prepare input failed: %d", (int)err);
        return err;
    }

    err = sr_bind_slot_io(ctx, slot_idx);
    if (err != RKVC_OK) {
        RKVC_LOG("rknn sr bind io failed: %d", (int)err);
        return err;
    }

    int ret = rknn_mem_sync(ctx->ctx, slot->input_mem,
                            RKNN_MEMORY_SYNC_TO_DEVICE);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("rknn mem sync to device failed: %d", ret);
        return RKVC_ERR_HW;
    }

    ret = rknn_run(ctx->ctx, NULL);
    if (ret != RKNN_SUCC) {
        RKVC_LOG("rknn_run failed: %d", ret);
        return RKVC_ERR_HW;
    }

    ctx->run_slot = slot_idx;
    ctx->busy = 1;
    return RKVC_OK;
}

rkvc_err rkvc_rknn_sr_ctx_collect(rkvc_rknn_sr_ctx *ctx,
                                  rkvc_buffer **out, int block)
{
    if (!ctx || !out)
        return RKVC_ERR_INVALID;

    *out = NULL;
    if (!ctx->busy)
        return RKVC_ERR_AGAIN;

    rkvc_err err = sr_wait_slot(ctx, block);
    if (err != RKVC_OK)
        return err;

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
    if (!src || !dst || dst_w <= 0 || dst_h <= 0)
        return RKVC_ERR_INVALID;
    if (!model_path || !model_path[0])
        return RKVC_ERR_INVALID;

    rkvc_rknn_sr_ctx *ctx = rkvc_rknn_sr_ctx_create(model_path, dst_w, dst_h,
                                                    NULL);
    if (!ctx)
        return RKVC_ERR_HW;

    rkvc_err err = rkvc_rknn_sr_ctx_process(ctx, src, dst);
    rkvc_rknn_sr_ctx_destroy(ctx);
    return err;
}

#else /* !RKVC_ENABLE_RKNN */

int rkvc_rknn_sr_available(void) { return 0; }

rkvc_rknn_sr_ctx *rkvc_rknn_sr_ctx_create(const char *model_path,
                                          int expect_out_w, int expect_out_h,
                                          rkvc_buffer_pool *pool)
{
    (void)model_path;
    (void)expect_out_w;
    (void)expect_out_h;
    (void)pool;
    return NULL;
}

void rkvc_rknn_sr_ctx_destroy(rkvc_rknn_sr_ctx *ctx)
{
    (void)ctx;
}

int rkvc_rknn_sr_ctx_busy(const rkvc_rknn_sr_ctx *ctx)
{
    (void)ctx;
    return 0;
}

void rkvc_rknn_sr_ctx_drain(rkvc_rknn_sr_ctx *ctx)
{
    (void)ctx;
}

rkvc_err rkvc_rknn_sr_ctx_submit(rkvc_rknn_sr_ctx *ctx,
                                 const rkvc_buffer *src)
{
    (void)ctx;
    (void)src;
    return RKVC_ERR_HW;
}

rkvc_err rkvc_rknn_sr_ctx_collect(rkvc_rknn_sr_ctx *ctx,
                                  rkvc_buffer **out, int block)
{
    (void)ctx;
    (void)out;
    (void)block;
    return RKVC_ERR_HW;
}

rkvc_err rkvc_rknn_sr_ctx_process(rkvc_rknn_sr_ctx *ctx,
                                  const rkvc_buffer *src,
                                  rkvc_buffer **out)
{
    (void)ctx;
    (void)src;
    (void)out;
    return RKVC_ERR_HW;
}

rkvc_err rkvc_rknn_sr_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                             int dst_w, int dst_h, const char *model_path)
{
    (void)src;
    (void)dst;
    (void)dst_w;
    (void)dst_h;
    (void)model_path;
    return RKVC_ERR_HW;
}

#endif /* RKVC_ENABLE_RKNN */
