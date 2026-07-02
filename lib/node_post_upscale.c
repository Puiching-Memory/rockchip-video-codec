/**
 * @file node_post_upscale.c
 * @brief 解码后上采样节点（RGA 硬件插值）。
 */

#include "internal.h"

const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo)
{
    switch (algo) {
    case RKVC_UPSCALE_NEAREST:  return "nearest";
    case RKVC_UPSCALE_BILINEAR: return "bilinear";
    case RKVC_UPSCALE_BICUBIC:  return "bicubic";
    case RKVC_UPSCALE_AI_SR:    return "rkvc_sr";
    default:                    return "none";
    }
}

int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out)
{
    if (!name || !out)
        return -1;
    if (strcmp(name, "none") == 0) {
        *out = RKVC_UPSCALE_NONE;
        return 0;
    }
    if (strcmp(name, "nearest") == 0) {
        *out = RKVC_UPSCALE_NEAREST;
        return 0;
    }
    if (strcmp(name, "bilinear") == 0) {
        *out = RKVC_UPSCALE_BILINEAR;
        return 0;
    }
    if (strcmp(name, "bicubic") == 0) {
        *out = RKVC_UPSCALE_BICUBIC;
        return 0;
    }
    if (strcmp(name, "rkvc_sr") == 0) {
        *out = RKVC_UPSCALE_AI_SR;
        return 0;
    }
    return -1;
}

static rkvc_err yuv420p_to_nv12_buffer(const rkvc_buffer *src, rkvc_buffer **nv12)
{
    if (!src->av_frame || src->format != RKVC_PIX_FMT_YUV420P)
        return RKVC_ERR_FORMAT;

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_buffer_alloc_video_host(&out,
                                                src->av_frame->width,
                                                src->av_frame->height,
                                                RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    const AVFrame *s = src->av_frame;
    AVFrame *d = out->av_frame;
    const int w = s->width;
    const int h = s->height;

    for (int y = 0; y < h; y++)
        memcpy(d->data[0] + y * d->linesize[0],
               s->data[0] + y * s->linesize[0], (size_t)w);

    const int ch = h / 2;
    const int cw = w / 2;
    for (int y = 0; y < ch; y++) {
        uint8_t *dd = d->data[1] + y * d->linesize[1];
        const uint8_t *u = s->data[1] + y * s->linesize[1];
        const uint8_t *v = s->data[2] + y * s->linesize[2];
        for (int x = 0; x < cw; x++) {
            dd[2 * x]     = u[x];
            dd[2 * x + 1] = v[x];
        }
    }

    out->pts = src->pts;
    *nv12 = out;
    return RKVC_OK;
}

static rkvc_err nv12_to_yuv420p_buffer(const rkvc_buffer *src, rkvc_buffer **yuv)
{
    if (!src->av_frame || src->format != RKVC_PIX_FMT_NV12)
        return RKVC_ERR_FORMAT;

    rkvc_buffer *out = NULL;
    rkvc_err err = rkvc_buffer_alloc_video_host(&out,
                                                src->av_frame->width,
                                                src->av_frame->height,
                                                RKVC_PIX_FMT_YUV420P);
    if (err != RKVC_OK)
        return err;

    const AVFrame *s = src->av_frame;
    AVFrame *d = out->av_frame;
    const int w = s->width;
    const int h = s->height;

    for (int y = 0; y < h; y++)
        memcpy(d->data[0] + y * d->linesize[0],
               s->data[0] + y * s->linesize[0], (size_t)w);

    const int ch = h / 2;
    const int cw = w / 2;
    for (int y = 0; y < ch; y++) {
        const uint8_t *ss = s->data[1] + y * s->linesize[1];
        uint8_t *u = d->data[1] + y * d->linesize[1];
        uint8_t *v = d->data[2] + y * d->linesize[2];
        for (int x = 0; x < cw; x++) {
            u[x] = ss[2 * x];
            v[x] = ss[2 * x + 1];
        }
    }

    out->pts = src->pts;
    *yuv = out;
    return RKVC_OK;
}

rkvc_err rkvc_post_upscale_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                                  int dst_w, int dst_h,
                                  rkvc_upscale_algo algo,
                                  const char *rkvc_sr_model_path)
{
    if (!src || !dst || dst_w <= 0 || dst_h <= 0)
        return RKVC_ERR_INVALID;
    if (algo == RKVC_UPSCALE_NONE)
        return RKVC_ERR_INVALID;

    *dst = NULL;

    if (!src->av_frame)
        return RKVC_ERR_INVALID;

    if (algo == RKVC_UPSCALE_AI_SR) {
        if (!rkvc_sr_model_path || !rkvc_sr_model_path[0])
            return RKVC_ERR_INVALID;
        return rkvc_rknn_sr_buffer(src, dst, dst_w, dst_h, rkvc_sr_model_path);
    }

    if (!rkvc_rga_available())
        return RKVC_ERR_HW;

    const int sw = src->av_frame->width;
    const int sh = src->av_frame->height;
    const rkvc_pix_fmt src_fmt = src->format;
    const rkvc_pix_fmt dst_fmt = src_fmt;

    if (sw == dst_w && sh == dst_h) {
        *dst = rkvc_buffer_ref((rkvc_buffer *)src);
        return RKVC_OK;
    }

    rkvc_buffer *nv12_src = NULL;
    rkvc_buffer *scaled = NULL;
    rkvc_err err = RKVC_OK;

    if (src_fmt == RKVC_PIX_FMT_NV12) {
        nv12_src = rkvc_buffer_ref((rkvc_buffer *)src);
    } else if (src_fmt == RKVC_PIX_FMT_YUV420P) {
        err = yuv420p_to_nv12_buffer(src, &nv12_src);
    } else {
        return RKVC_ERR_FORMAT;
    }

    if (err == RKVC_OK)
        err = rkvc_rga_scale_buffer(nv12_src, &scaled, dst_w, dst_h,
                                    RKVC_PIX_FMT_NV12, algo);

    rkvc_buffer_unref(nv12_src);

    if (err != RKVC_OK)
        return err;

    if (dst_fmt == RKVC_PIX_FMT_YUV420P) {
        err = nv12_to_yuv420p_buffer(scaled, dst);
        rkvc_buffer_unref(scaled);
        return err;
    }

    scaled->pts = src->pts;
    *dst = scaled;
    return RKVC_OK;
}
