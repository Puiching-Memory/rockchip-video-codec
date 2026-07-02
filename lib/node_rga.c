/**
 * @file node_rga.c
 * @brief RGA NV12 硬件缩放（importbuffer → imcheck → imresize → release）。
 */

#include "internal.h"

#include <rga/im2d.h>
#include <rga/rga.h>
#include <rga/RgaUtils.h>
#include <pthread.h>
#include <sys/stat.h>

static int g_rga_available = -1;
static pthread_once_t g_rga_once = PTHREAD_ONCE_INIT;

static void detect_rga(void)
{
    struct stat st;
    g_rga_available = (stat("/dev/rga", &st) == 0) ? 1 : 0;
}

int rkvc_rga_available(void)
{
    pthread_once(&g_rga_once, detect_rga);
    return g_rga_available;
}

static int frame_contiguous(const AVFrame *f)
{
    if (!f || !f->data[0] || f->format != AV_PIX_FMT_NV12 || !f->data[1])
        return 0;
    return (f->data[1] == f->data[0] + (ptrdiff_t)f->linesize[0] * f->height)
        && (f->linesize[1] == f->linesize[0]);
}

static int rga_nv12_stride_ok(int wstride, int height)
{
    if (wstride <= 0 || height <= 0)
        return 0;
    if ((wstride & 1) || (height & 1))
        return 0;
    if ((wstride & 15) != 0)
        return 0;
    return 1;
}

static IM_SCALE_MODE rkvc_upscale_to_rga_mode(rkvc_upscale_algo algo)
{
    switch (algo) {
    case RKVC_UPSCALE_NEAREST:  return INTER_NEAREST;
    case RKVC_UPSCALE_BICUBIC:  return INTER_CUBIC;
    case RKVC_UPSCALE_BILINEAR:
    default:                    return INTER_LINEAR;
    }
}

typedef struct {
    rga_buffer_handle_t handle;
    int                 owned;
} rga_import_t;

static void rga_import_release(rga_import_t *imp)
{
    if (imp && imp->owned && imp->handle)
        releasebuffer_handle(imp->handle);
    if (imp) {
        imp->handle = 0;
        imp->owned  = 0;
    }
}

static int buffer_nv12_rga_stride(const rkvc_buffer *buf, int *wstride, int *hstride)
{
    if (!buf || !buf->av_frame || !wstride || !hstride)
        return 0;

    const int w = buf->av_frame->width;
    const int h = buf->av_frame->height;
    if (w <= 0 || h <= 0)
        return 0;

    int ws = buf->av_frame->linesize[0];
    if (ws <= 0 && buf->strides[0] > 0)
        ws = (int)buf->strides[0];
    if (ws <= 0)
        ws = w;

    *wstride = ws;
    *hstride = h;
    return 1;
}

static rkvc_err rga_import_nv12_buffer(const rkvc_buffer *buf, int w, int h,
                                       int wstride, int hstride,
                                       rga_import_t *imp, rga_buffer_t *rga)
{
    if (!buf || !buf->av_frame || !imp || !rga)
        return RKVC_ERR_INVALID;
    if (!rga_nv12_stride_ok(wstride, hstride))
        return RKVC_ERR_FORMAT;

    memset(imp, 0, sizeof(*imp));
    memset(rga, 0, sizeof(*rga));

    const int fmt = RK_FORMAT_YCbCr_420_SP;
    im_handle_param_t param = {
        .width  = (uint32_t)w,
        .height = (uint32_t)h,
        .format = (uint32_t)fmt,
    };

    if (buf->mem_type == RKVC_MEM_DMABUF && buf->fd >= 0) {
        imp->handle = importbuffer_fd(buf->fd, &param);
    } else if (frame_contiguous(buf->av_frame)) {
        imp->handle = importbuffer_virtualaddr(buf->av_frame->data[0], &param);
    } else {
        return RKVC_ERR_FORMAT;
    }

    if (!imp->handle)
        return RKVC_ERR_HW;

    imp->owned = 1;
    *rga = wrapbuffer_handle_t(imp->handle, w, h, wstride, hstride, fmt);
    return RKVC_OK;
}

static rkvc_err nv12_copy_contiguous(const rkvc_buffer *src, rkvc_buffer **dst)
{
    if (!src->av_frame || src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_buffer_pool_alloc_video(NULL, &out,
                                                src->av_frame->width,
                                                src->av_frame->height,
                                                RKVC_PIX_FMT_NV12,
                                                RKVC_MEM_DMABUF);
    if (err != RKVC_OK) {
        err = rkvc_buffer_alloc_video_host(&out,
                                           src->av_frame->width,
                                           src->av_frame->height,
                                           RKVC_PIX_FMT_NV12);
        if (err != RKVC_OK)
            return err;
    }

    const AVFrame *s = src->av_frame;
    AVFrame *d = out->av_frame;
    av_image_copy(d->data, d->linesize,
                  (const uint8_t *const *)s->data, s->linesize,
                  AV_PIX_FMT_NV12, s->width, s->height);

    out->pts = src->pts;
    *dst = out;
    return RKVC_OK;
}

static rkvc_err rga_resize_checked(const rga_buffer_t *src, const rga_buffer_t *dst,
                                   IM_SCALE_MODE mode)
{
    im_rect src_rect = {0, 0, src->width, src->height};
    im_rect dst_rect = {0, 0, dst->width, dst->height};
    rga_buffer_t pat;
    im_rect pat_rect;

    memset(&pat, 0, sizeof(pat));
    memset(&pat_rect, 0, sizeof(pat_rect));

    IM_STATUS chk = imcheck_t(*src, *dst, pat, src_rect, dst_rect, pat_rect, 0);
    if (chk != IM_STATUS_NOERROR) {
        RKVC_LOG("RGA imcheck failed: %s", imStrError_t(chk));
        return RKVC_ERR_HW;
    }

    IM_STATUS ret = imresize_t(*src, *dst, 0, 0, mode, 1);
    if (ret != IM_STATUS_SUCCESS) {
        RKVC_LOG("RGA imresize failed: %s", imStrError_t(ret));
        return RKVC_ERR_HW;
    }
    return RKVC_OK;
}

static rkvc_err rga_scale_nv12_virtual(const uint8_t *src, const uint8_t *dst,
                                       int src_w, int src_h,
                                       int dst_w, int dst_h,
                                       IM_SCALE_MODE mode)
{
    if (!src || !dst)
        return RKVC_ERR_INVALID;
    if (!rga_nv12_stride_ok(src_w, src_h) || !rga_nv12_stride_ok(dst_w, dst_h))
        return RKVC_ERR_FORMAT;

    rga_import_t src_imp = {0};
    rga_import_t dst_imp = {0};
    rga_buffer_t rga_src;
    rga_buffer_t rga_dst;
    rkvc_err err = RKVC_OK;

    const int fmt = RK_FORMAT_YCbCr_420_SP;
    im_handle_param_t src_param = {
        .width  = (uint32_t)src_w,
        .height = (uint32_t)src_h,
        .format = (uint32_t)fmt,
    };
    im_handle_param_t dst_param = {
        .width  = (uint32_t)dst_w,
        .height = (uint32_t)dst_h,
        .format = (uint32_t)fmt,
    };

    src_imp.handle = importbuffer_virtualaddr((void *)(uintptr_t)src, &src_param);
    dst_imp.handle = importbuffer_virtualaddr((void *)(uintptr_t)dst, &dst_param);
    if (!src_imp.handle || !dst_imp.handle) {
        err = RKVC_ERR_HW;
        goto done;
    }

    src_imp.owned = 1;
    dst_imp.owned = 1;
    rga_src = wrapbuffer_handle_t(src_imp.handle, src_w, src_h, src_w, src_h, fmt);
    rga_dst = wrapbuffer_handle_t(dst_imp.handle, dst_w, dst_h, dst_w, dst_h, fmt);
    err = rga_resize_checked(&rga_src, &rga_dst, mode);

done:
    rga_import_release(&src_imp);
    rga_import_release(&dst_imp);
    return err;
}

static rkvc_err rga_scale_nv12_buffers(const rkvc_buffer *src, rkvc_buffer *dst,
                                       IM_SCALE_MODE mode)
{
    const AVFrame *sf = src->av_frame;
    const AVFrame *df = dst->av_frame;
    const int sw = sf->width;
    const int sh = sf->height;
    const int dw = df->width;
    const int dh = df->height;
    int src_ws = 0, src_hs = 0, dst_ws = 0, dst_hs = 0;

    if (!buffer_nv12_rga_stride(src, &src_ws, &src_hs) ||
        !buffer_nv12_rga_stride(dst, &dst_ws, &dst_hs))
        return RKVC_ERR_FORMAT;

    rga_import_t src_imp = {0};
    rga_import_t dst_imp = {0};
    rga_buffer_t rga_src;
    rga_buffer_t rga_dst;
    rkvc_err err;

    err = rga_import_nv12_buffer(src, sw, sh, src_ws, src_hs,
                                 &src_imp, &rga_src);
    if (err != RKVC_OK)
        goto done;

    err = rga_import_nv12_buffer(dst, dw, dh, dst_ws, dst_hs,
                                 &dst_imp, &rga_dst);
    if (err != RKVC_OK)
        goto done;

    err = rga_resize_checked(&rga_src, &rga_dst, mode);

done:
    rga_import_release(&src_imp);
    rga_import_release(&dst_imp);
    return err;
}

rkvc_err rkvc_rga_scale_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                               int dst_w, int dst_h, rkvc_pix_fmt dst_fmt,
                               rkvc_upscale_algo algo)
{
    if (!src || !dst || dst_w <= 0 || dst_h <= 0)
        return RKVC_ERR_INVALID;
    if (!rkvc_rga_available())
        return RKVC_ERR_HW;

    *dst = NULL;

    if (src->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;

    if (!src->av_frame)
        return RKVC_ERR_INVALID;

    const int sw = src->av_frame->width;
    const int sh = src->av_frame->height;
    const rkvc_pix_fmt src_fmt = src->format;

    if (sw == dst_w && sh == dst_h && src_fmt == dst_fmt) {
        *dst = rkvc_buffer_ref((rkvc_buffer *)src);
        return RKVC_OK;
    }

    if (src_fmt != RKVC_PIX_FMT_NV12 || dst_fmt != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;

    rkvc_buffer *work = NULL;
    const rkvc_buffer *rga_src = src;

    if (src->mem_type != RKVC_MEM_DMABUF && !frame_contiguous(src->av_frame)) {
        rkvc_err err = nv12_copy_contiguous(src, &work);
        if (err != RKVC_OK)
            return err;
        rga_src = work;
    }

    int src_ws = 0, src_hs = 0;
    if (!buffer_nv12_rga_stride(rga_src, &src_ws, &src_hs) ||
        !rga_nv12_stride_ok(src_ws, src_hs) ||
        !rga_nv12_stride_ok(dst_w, dst_h)) {
        rkvc_buffer_unref(work);
        return RKVC_ERR_FORMAT;
    }

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_buffer_pool_alloc_video(NULL, &out, dst_w, dst_h,
                                                RKVC_PIX_FMT_NV12,
                                                RKVC_MEM_DMABUF);
    if (err != RKVC_OK) {
        err = rkvc_buffer_alloc_video_host(&out, dst_w, dst_h,
                                           RKVC_PIX_FMT_NV12);
        if (err != RKVC_OK) {
            rkvc_buffer_unref(work);
            return err;
        }
    }

    err = rga_scale_nv12_buffers(rga_src, out, rkvc_upscale_to_rga_mode(algo));
    rkvc_buffer_unref(work);

    if (err != RKVC_OK) {
        rkvc_buffer_unref(out);
        return err;
    }

    out->pts = src->pts;
    *dst = out;
    return RKVC_OK;
}

static void nv12_copy_tight_to_frame(const uint8_t *src, AVFrame *f, int w, int h)
{
    const int ys = f->linesize[0];
    const size_t uv_off = (size_t)w * (size_t)h;
    for (int y = 0; y < h; y++)
        memcpy(f->data[0] + y * ys, src + (size_t)y * (size_t)w, (size_t)w);

    const int ch = h / 2;
    const int us = f->linesize[1];
    for (int y = 0; y < ch; y++)
        memcpy(f->data[1] + y * us, src + uv_off + (size_t)y * (size_t)w, (size_t)w);
}

static void nv12_copy_frame_to_tight(const AVFrame *f, uint8_t *dst, int w, int h)
{
    const int ys = f->linesize[0];
    const size_t uv_off = (size_t)w * (size_t)h;
    for (int y = 0; y < h; y++)
        memcpy(dst + (size_t)y * (size_t)w, f->data[0] + y * ys, (size_t)w);

    const int ch = h / 2;
    const int us = f->linesize[1];
    for (int y = 0; y < ch; y++)
        memcpy(dst + uv_off + (size_t)y * (size_t)w, f->data[1] + y * us, (size_t)w);
}

rkvc_err rkvc_upscale_nv12(const uint8_t *src, uint8_t *dst,
                           int src_w, int src_h,
                           int dst_w, int dst_h,
                           rkvc_upscale_algo algo)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return RKVC_ERR_INVALID;
    if (algo == RKVC_UPSCALE_NONE)
        return RKVC_ERR_INVALID;
    if (!rkvc_rga_available())
        return RKVC_ERR_HW;

    const IM_SCALE_MODE mode = rkvc_upscale_to_rga_mode(algo);

    /* 紧凑 NV12 + RGA 对齐 stride：直接 import 用户缓冲，零拷贝缩放。 */
    rkvc_err err = rga_scale_nv12_virtual(src, dst, src_w, src_h, dst_w, dst_h,
                                          mode);
    if (err == RKVC_OK)
        return RKVC_OK;

    if (!rga_nv12_stride_ok(src_w, src_h) || !rga_nv12_stride_ok(dst_w, dst_h))
        return RKVC_ERR_FORMAT;

    rkvc_buffer *nv12_src = NULL;
    rkvc_buffer *scaled = NULL;

    err = rkvc_buffer_alloc_video_host(&nv12_src, src_w, src_h,
                                       RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    nv12_copy_tight_to_frame(src, nv12_src->av_frame, src_w, src_h);
    err = rkvc_rga_scale_buffer(nv12_src, &scaled, dst_w, dst_h,
                                RKVC_PIX_FMT_NV12, algo);
    if (err == RKVC_OK)
        nv12_copy_frame_to_tight(scaled->av_frame, dst, dst_w, dst_h);

    rkvc_buffer_unref(scaled);
    rkvc_buffer_unref(nv12_src);
    return err;
}

static void yuv420p_to_nv12_frame(const AVFrame *src, AVFrame *dst)
{
    const int w = src->width;
    const int h = src->height;
    for (int y = 0; y < h; y++)
        memcpy(dst->data[0] + y * dst->linesize[0],
               src->data[0] + y * src->linesize[0], (size_t)w);

    const int ch = h / 2;
    const int cw = w / 2;
    for (int y = 0; y < ch; y++) {
        uint8_t *d = dst->data[1] + y * dst->linesize[1];
        const uint8_t *u = src->data[1] + y * src->linesize[1];
        const uint8_t *v = src->data[2] + y * src->linesize[2];
        for (int x = 0; x < cw; x++) {
            d[2 * x]     = u[x];
            d[2 * x + 1] = v[x];
        }
    }
}

static void nv12_to_yuv420p_frame(const AVFrame *src, AVFrame *dst)
{
    const int w = src->width;
    const int h = src->height;
    for (int y = 0; y < h; y++)
        memcpy(dst->data[0] + y * dst->linesize[0],
               src->data[0] + y * src->linesize[0], (size_t)w);

    const int ch = h / 2;
    const int cw = w / 2;
    for (int y = 0; y < ch; y++) {
        const uint8_t *s = src->data[1] + y * src->linesize[1];
        uint8_t *u = dst->data[1] + y * dst->linesize[1];
        uint8_t *v = dst->data[2] + y * dst->linesize[2];
        for (int x = 0; x < cw; x++) {
            u[x] = s[2 * x];
            v[x] = s[2 * x + 1];
        }
    }
}

rkvc_err rkvc_upscale_yuv420p(const uint8_t *src, uint8_t *dst,
                              int src_w, int src_h,
                              int dst_w, int dst_h,
                              rkvc_upscale_algo algo)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return RKVC_ERR_INVALID;
    if (algo == RKVC_UPSCALE_NONE)
        return RKVC_ERR_INVALID;
    if (!rkvc_rga_available())
        return RKVC_ERR_HW;

    AVFrame yuv_src;
    AVFrame yuv_dst;
    memset(&yuv_src, 0, sizeof(yuv_src));
    memset(&yuv_dst, 0, sizeof(yuv_dst));
    yuv_src.format = AV_PIX_FMT_YUV420P;
    yuv_src.width  = src_w;
    yuv_src.height = src_h;
    yuv_dst.format = AV_PIX_FMT_YUV420P;
    yuv_dst.width  = dst_w;
    yuv_dst.height = dst_h;

    if (av_image_fill_arrays(yuv_src.data, yuv_src.linesize, src,
                             AV_PIX_FMT_YUV420P, src_w, src_h, 1) < 0 ||
        av_image_fill_arrays(yuv_dst.data, yuv_dst.linesize, dst,
                             AV_PIX_FMT_YUV420P, dst_w, dst_h, 1) < 0)
        return RKVC_ERR_FORMAT;

    rkvc_buffer *nv12_src = NULL;
    rkvc_buffer *scaled = NULL;
    rkvc_err err = rkvc_buffer_alloc_video_host(&nv12_src, src_w, src_h,
                                                RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    yuv420p_to_nv12_frame(&yuv_src, nv12_src->av_frame);
    err = rkvc_rga_scale_buffer(nv12_src, &scaled, dst_w, dst_h,
                                RKVC_PIX_FMT_NV12, algo);
    if (err == RKVC_OK)
        nv12_to_yuv420p_frame(scaled->av_frame, &yuv_dst);

    rkvc_buffer_unref(scaled);
    rkvc_buffer_unref(nv12_src);
    return err;
}

struct rkvc_upscale_ctx {
    int           src_w;
    int           src_h;
    int           dst_w;
    int           dst_h;
    IM_SCALE_MODE mode;
    uint8_t      *src_buf;
    uint8_t      *dst_buf;
    size_t        src_bytes;
    size_t        dst_bytes;
    rga_import_t  src_imp;
    rga_import_t  dst_imp;
    rga_buffer_t  rga_src;
    rga_buffer_t  rga_dst;
};

rkvc_upscale_ctx *rkvc_upscale_ctx_create(int src_w, int src_h,
                                          int dst_w, int dst_h,
                                          rkvc_upscale_algo algo)
{
    if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return NULL;
    if (algo == RKVC_UPSCALE_NONE)
        return NULL;
    if (!rkvc_rga_available())
        return NULL;
    if (!rga_nv12_stride_ok(src_w, src_h) || !rga_nv12_stride_ok(dst_w, dst_h))
        return NULL;

    rkvc_upscale_ctx *ctx = rkvc_calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->src_w     = src_w;
    ctx->src_h     = src_h;
    ctx->dst_w     = dst_w;
    ctx->dst_h     = dst_h;
    ctx->mode      = rkvc_upscale_to_rga_mode(algo);
    ctx->src_bytes = (size_t)src_w * (size_t)src_h * 3 / 2;
    ctx->dst_bytes = (size_t)dst_w * (size_t)dst_h * 3 / 2;

    ctx->src_buf = rkvc_malloc(ctx->src_bytes);
    ctx->dst_buf = rkvc_malloc(ctx->dst_bytes);
    if (!ctx->src_buf || !ctx->dst_buf)
        goto fail;

    const int fmt = RK_FORMAT_YCbCr_420_SP;
    im_handle_param_t src_param = {
        .width  = (uint32_t)src_w,
        .height = (uint32_t)src_h,
        .format = (uint32_t)fmt,
    };
    im_handle_param_t dst_param = {
        .width  = (uint32_t)dst_w,
        .height = (uint32_t)dst_h,
        .format = (uint32_t)fmt,
    };

    ctx->src_imp.handle = importbuffer_virtualaddr(ctx->src_buf, &src_param);
    ctx->dst_imp.handle = importbuffer_virtualaddr(ctx->dst_buf, &dst_param);
    if (!ctx->src_imp.handle || !ctx->dst_imp.handle)
        goto fail;

    ctx->src_imp.owned = 1;
    ctx->dst_imp.owned = 1;
    ctx->rga_src = wrapbuffer_handle_t(ctx->src_imp.handle, src_w, src_h,
                                       src_w, src_h, fmt);
    ctx->rga_dst = wrapbuffer_handle_t(ctx->dst_imp.handle, dst_w, dst_h,
                                       dst_w, dst_h, fmt);
    return ctx;

fail:
    rkvc_upscale_ctx_destroy(ctx);
    return NULL;
}

void rkvc_upscale_ctx_destroy(rkvc_upscale_ctx *ctx)
{
    if (!ctx)
        return;
    rga_import_release(&ctx->src_imp);
    rga_import_release(&ctx->dst_imp);
    rkvc_free(ctx->src_buf);
    rkvc_free(ctx->dst_buf);
    rkvc_free(ctx);
}

uint8_t *rkvc_upscale_ctx_src_buf(rkvc_upscale_ctx *ctx)
{
    return ctx ? ctx->src_buf : NULL;
}

uint8_t *rkvc_upscale_ctx_dst_buf(rkvc_upscale_ctx *ctx)
{
    return ctx ? ctx->dst_buf : NULL;
}

size_t rkvc_upscale_ctx_src_bytes(const rkvc_upscale_ctx *ctx)
{
    return ctx ? ctx->src_bytes : 0;
}

size_t rkvc_upscale_ctx_dst_bytes(const rkvc_upscale_ctx *ctx)
{
    return ctx ? ctx->dst_bytes : 0;
}

rkvc_err rkvc_upscale_ctx_process(rkvc_upscale_ctx *ctx)
{
    if (!ctx || !ctx->src_buf || !ctx->dst_buf)
        return RKVC_ERR_INVALID;
    return rga_resize_checked(&ctx->rga_src, &ctx->rga_dst, ctx->mode);
}

struct rkvc_rga_scale_ctx {
    rkvc_buffer  *dst;
    rga_import_t  dst_imp;
    rga_buffer_t  rga_dst;
    int           dst_w;
    int           dst_h;
    IM_SCALE_MODE mode;
};

rkvc_rga_scale_ctx *rkvc_rga_scale_ctx_create(int dst_w, int dst_h,
                                              rkvc_upscale_algo algo)
{
    if (dst_w <= 0 || dst_h <= 0 || algo == RKVC_UPSCALE_NONE)
        return NULL;
    if (!rkvc_rga_available())
        return NULL;
    if (!rga_nv12_stride_ok(dst_w, dst_h))
        return NULL;

    rkvc_rga_scale_ctx *ctx = rkvc_calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->dst_w = dst_w;
    ctx->dst_h = dst_h;
    ctx->mode  = rkvc_upscale_to_rga_mode(algo);

    rkvc_err err = rkvc_buffer_pool_alloc_video(NULL, &ctx->dst, dst_w, dst_h,
                                                RKVC_PIX_FMT_NV12,
                                                RKVC_MEM_DMABUF);
    if (err != RKVC_OK)
        err = rkvc_buffer_alloc_video_host(&ctx->dst, dst_w, dst_h,
                                           RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        goto fail;

    int dst_ws = 0, dst_hs = 0;
    if (!buffer_nv12_rga_stride(ctx->dst, &dst_ws, &dst_hs)) {
        err = RKVC_ERR_FORMAT;
        goto fail;
    }

    if (!frame_contiguous(ctx->dst->av_frame)) {
        err = RKVC_ERR_FORMAT;
        goto fail;
    }

    const int fmt = RK_FORMAT_YCbCr_420_SP;
    im_handle_param_t param = {
        .width  = (uint32_t)dst_w,
        .height = (uint32_t)dst_h,
        .format = (uint32_t)fmt,
    };
    ctx->dst_imp.handle =
        importbuffer_virtualaddr(ctx->dst->av_frame->data[0], &param);
    if (!ctx->dst_imp.handle) {
        err = RKVC_ERR_HW;
        goto fail;
    }
    ctx->dst_imp.owned = 1;
    ctx->rga_dst = wrapbuffer_handle_t(ctx->dst_imp.handle, dst_w, dst_h,
                                       dst_ws, dst_hs, fmt);
    return ctx;

fail:
    rkvc_rga_scale_ctx_destroy(ctx);
    return NULL;
}

void rkvc_rga_scale_ctx_destroy(rkvc_rga_scale_ctx *ctx)
{
    if (!ctx)
        return;
    rga_import_release(&ctx->dst_imp);
    rkvc_buffer_unref(ctx->dst);
    rkvc_free(ctx);
}

rkvc_err rkvc_rga_scale_ctx_process(rkvc_rga_scale_ctx *ctx,
                                  const rkvc_buffer *src,
                                  rkvc_buffer **out)
{
    if (!ctx || !src || !out || !ctx->dst)
        return RKVC_ERR_INVALID;

    *out = NULL;
    if (!src->av_frame || src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;

    const int sw = src->av_frame->width;
    const int sh = src->av_frame->height;
    if (sw == ctx->dst_w && sh == ctx->dst_h) {
        *out = rkvc_buffer_ref((rkvc_buffer *)src);
        return RKVC_OK;
    }

    rkvc_buffer *work = NULL;
    const rkvc_buffer *rga_src = src;

    if (src->mem_type != RKVC_MEM_DMABUF && !frame_contiguous(src->av_frame)) {
        rkvc_err err = nv12_copy_contiguous(src, &work);
        if (err != RKVC_OK)
            return err;
        rga_src = work;
    }

    int src_ws = 0, src_hs = 0;
    if (!buffer_nv12_rga_stride(rga_src, &src_ws, &src_hs) ||
        !rga_nv12_stride_ok(src_ws, src_hs)) {
        rkvc_buffer_unref(work);
        return RKVC_ERR_FORMAT;
    }

    rga_import_t src_imp = {0};
    rga_buffer_t rga_src_buf;
    rkvc_err err = rga_import_nv12_buffer(rga_src, sw, sh, src_ws, src_hs,
                                          &src_imp, &rga_src_buf);
    if (err != RKVC_OK) {
        rkvc_buffer_unref(work);
        return err;
    }

    err = rkvc_buffer_dmabuf_begin_device_write(ctx->dst);
    if (err != RKVC_OK) {
        rga_import_release(&src_imp);
        rkvc_buffer_unref(work);
        return err;
    }

    err = rga_resize_checked(&rga_src_buf, &ctx->rga_dst, ctx->mode);
    rga_import_release(&src_imp);
    rkvc_buffer_unref(work);

    if (err != RKVC_OK)
        return err;

    err = rkvc_buffer_dmabuf_end_device_write(ctx->dst);
    if (err != RKVC_OK)
        return err;

    ctx->dst->pts = src->pts;
    *out = rkvc_buffer_ref(ctx->dst);
    return RKVC_OK;
}

int rkvc_rga_rgb888_stride(int width)
{
    if (width <= 0)
        return 0;
    /* RGA RGB888 的 wstride 为像素跨度，须 16 像素对齐 */
    return (width + 15) & ~15;
}

int rkvc_rga_rgb888_row_bytes(int width)
{
    const int ps = rkvc_rga_rgb888_stride(width);
    return ps > 0 ? ps * 3 : 0;
}

static rkvc_err rga_cvtcolor_checked(const rga_buffer_t *src,
                                     const rga_buffer_t *dst,
                                     int sfmt, int dfmt)
{
    IM_STATUS ret = imcvtcolor_t(*src, *dst, sfmt, dfmt,
                                 IM_COLOR_SPACE_DEFAULT, 1);
    if (ret != IM_STATUS_SUCCESS) {
        RKVC_LOG("RGA imcvtcolor failed: %s (src %dx%d fmt=%#x -> dst %dx%d fmt=%#x)",
                 imStrError_t(ret), src->width, src->height, sfmt,
                 dst->width, dst->height, dfmt);
        return RKVC_ERR_HW;
    }
    return RKVC_OK;
}

static rkvc_err rga_import_rgb888_virtual(const uint8_t *rgb, int w, int h,
                                          int stride_pixels, rga_import_t *imp,
                                          rga_buffer_t *rga)
{
    if (!rgb || !imp || !rga || w <= 0 || h <= 0 ||
        stride_pixels < rkvc_rga_rgb888_stride(w))
        return RKVC_ERR_INVALID;

    memset(imp, 0, sizeof(*imp));
    memset(rga, 0, sizeof(*rga));

    const int fmt = RK_FORMAT_RGB_888;
    im_handle_param_t param = {
        .width  = (uint32_t)w,
        .height = (uint32_t)h,
        .format = (uint32_t)fmt,
    };

    imp->handle = importbuffer_virtualaddr((void *)(uintptr_t)rgb, &param);
    if (!imp->handle)
        return RKVC_ERR_HW;

    imp->owned = 1;
    *rga = wrapbuffer_handle_t(imp->handle, w, h, stride_pixels, h, fmt);
    return RKVC_OK;
}

rkvc_err rkvc_rga_csc_nv12_to_rgb888(const rkvc_buffer *src, uint8_t *rgb,
                                     int w, int h, int rgb_stride_pixels)
{
    if (!src || !src->av_frame || !rgb || w <= 0 || h <= 0)
        return RKVC_ERR_INVALID;
    if (src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;
    if (!rkvc_rga_available())
        return RKVC_ERR_HW;
    if (rgb_stride_pixels < rkvc_rga_rgb888_stride(w))
        return RKVC_ERR_INVALID;

    rkvc_buffer *work = NULL;
    const rkvc_buffer *nv12 = src;

    if (src->mem_type != RKVC_MEM_DMABUF && !frame_contiguous(src->av_frame)) {
        rkvc_err err = nv12_copy_contiguous(src, &work);
        if (err != RKVC_OK)
            return err;
        nv12 = work;
    }

    int src_ws = 0, src_hs = 0;
    if (!buffer_nv12_rga_stride(nv12, &src_ws, &src_hs) ||
        !rga_nv12_stride_ok(src_ws, src_hs)) {
        rkvc_buffer_unref(work);
        return RKVC_ERR_FORMAT;
    }

    rga_import_t src_imp = {0};
    rga_import_t dst_imp = {0};
    rga_buffer_t rga_src;
    rga_buffer_t rga_dst;
    rkvc_err err;

    err = rga_import_nv12_buffer(nv12, w, h, src_ws, src_hs,
                                 &src_imp, &rga_src);
    if (err != RKVC_OK)
        goto done;

    err = rga_import_rgb888_virtual(rgb, w, h, rgb_stride_pixels, &dst_imp, &rga_dst);
    if (err != RKVC_OK)
        goto done;

    err = rga_cvtcolor_checked(&rga_src, &rga_dst,
                               RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);

done:
    rga_import_release(&src_imp);
    rga_import_release(&dst_imp);
    rkvc_buffer_unref(work);
    return err;
}

rkvc_err rkvc_rga_csc_rgb888_to_nv12(const uint8_t *rgb, int w, int h,
                                     int rgb_stride_pixels, rkvc_buffer *dst)
{
    if (!rgb || !dst || !dst->av_frame || w <= 0 || h <= 0)
        return RKVC_ERR_INVALID;
    if (dst->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;
    if (!rkvc_rga_available())
        return RKVC_ERR_HW;
    if (rgb_stride_pixels < rkvc_rga_rgb888_stride(w))
        return RKVC_ERR_INVALID;

    int dst_ws = 0, dst_hs = 0;
    if (!buffer_nv12_rga_stride(dst, &dst_ws, &dst_hs) ||
        !rga_nv12_stride_ok(dst_ws, dst_hs))
        return RKVC_ERR_FORMAT;

    rga_import_t src_imp = {0};
    rga_import_t dst_imp = {0};
    rga_buffer_t rga_src;
    rga_buffer_t rga_dst;
    rkvc_err err;

    err = rga_import_rgb888_virtual(rgb, w, h, rgb_stride_pixels, &src_imp, &rga_src);
    if (err != RKVC_OK)
        goto done;

    err = rga_import_nv12_buffer(dst, w, h, dst_ws, dst_hs,
                                 &dst_imp, &rga_dst);
    if (err != RKVC_OK)
        goto done;

    err = rkvc_buffer_dmabuf_begin_device_write(dst);
    if (err != RKVC_OK)
        goto done;

    err = rga_cvtcolor_checked(&rga_src, &rga_dst,
                               RK_FORMAT_RGB_888, RK_FORMAT_YCbCr_420_SP);
    if (err == RKVC_OK)
        err = rkvc_buffer_dmabuf_end_device_write(dst);

done:
    rga_import_release(&src_imp);
    rga_import_release(&dst_imp);
    return err;
}
