/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session_scale.c
 * @brief Session 编码前下采样 / 解码后上采样 / NV12 写出。
 */

#include "internal.h"
#include "session_priv.h"

static int session_align2(int v)
{
    return v > 0 ? (v & ~1) : 0;
}

rkvc_err session_enc_size(const rkvc_session *s, int *ew, int *eh)
{
    int denom = s->desc.enc_scale_denom;
    int w = s->desc.width;
    int h = s->desc.height;
    if (denom <= 1) {
        *ew = w;
        *eh = h;
        return RKVC_OK;
    }
    *ew = session_align2(w / denom);
    *eh = session_align2(h / denom);
    /* 正分辨率再除 denom 后不得变成 0×0（align2 会把 1 收成 0）。 */
    if (w > 0 && h > 0 && (*ew < 2 || *eh < 2))
        return RKVC_ERR_INVALID;
    return RKVC_OK;
}

int session_needs_post_upscale(const rkvc_session *s)
{
    return s->desc.post_upscale_algo != RKVC_UPSCALE_NONE &&
           s->desc.enc_scale_denom > 1;
}

rkvc_err session_downscale_for_encode(rkvc_session *s,
                                             rkvc_buffer *frame,
                                             rkvc_buffer **out)
{
    int ew = 0, eh = 0;
    rkvc_err szerr = session_enc_size(s, &ew, &eh);
    if (szerr != RKVC_OK)
        return szerr;
    *out = frame;

    if ((int)frame->width == ew && (int)frame->height == eh)
        return RKVC_OK;

    rkvc_buffer *scaled = NULL;
    rkvc_err err = rkvc_rga_scale_buffer(frame, &scaled, ew, eh,
                                         s->desc.pixel_format,
                                         RKVC_UPSCALE_BILINEAR);
    if (err != RKVC_OK)
        return err;
    scaled->pts = frame->pts;
    *out = scaled;
    return RKVC_OK;
}

int session_uses_ai_sr(const rkvc_session *s)
{
    return s->desc.post_upscale_algo == RKVC_UPSCALE_AI_SR;
}

int session_can_rga_upscale_dmabuf(const rkvc_session *s,
                                         const rkvc_buffer *frame)
{
    return session_needs_post_upscale(s) &&
           !session_uses_ai_sr(s) &&
           rkvc_rga_available() &&
           frame->mem_type == RKVC_MEM_DMABUF &&
           frame->format == RKVC_PIX_FMT_NV12;
}

int session_can_ai_sr_dmabuf(const rkvc_session *s,
                                    const rkvc_buffer *frame)
{
    return session_needs_post_upscale(s) &&
           session_uses_ai_sr(s) &&
           rkvc_rga_available() &&
           frame->mem_type == RKVC_MEM_DMABUF &&
           frame->format == RKVC_PIX_FMT_NV12;
}

rkvc_err session_frame_to_host(const rkvc_session *s,
                                      rkvc_buffer *frame,
                                      rkvc_buffer **host)
{
    if (session_can_rga_upscale_dmabuf(s, frame) ||
        session_can_ai_sr_dmabuf(s, frame)) {
        *host = rkvc_buffer_ref(frame);
        return RKVC_OK;
    }
    if (frame->mem_type == RKVC_MEM_DMABUF)
        return rkvc_dma_to_host(frame, host);
    *host = rkvc_buffer_ref(frame);
    return RKVC_OK;
}

rkvc_err session_ensure_rknn_sr(rkvc_session *s)
{
    if (s->rknn_sr)
        return RKVC_OK;
    if (!s->desc.post_upscale_rkvc_model_path ||
        !s->desc.post_upscale_rkvc_model_path[0])
        return RKVC_ERR_INVALID;
    s->rknn_sr = rkvc_rknn_sr_ctx_create(s->desc.post_upscale_rkvc_model_path,
                                         s->desc.width, s->desc.height,
                                         s->pool);
    return s->rknn_sr ? RKVC_OK : RKVC_ERR_HW;
}

rkvc_err session_apply_post_upscale(rkvc_session *s,
                                           rkvc_buffer *host,
                                           rkvc_buffer **out)
{
    *out = host;
    if (!session_needs_post_upscale(s))
        return RKVC_OK;

    if (session_uses_ai_sr(s)) {
        rkvc_err err = session_ensure_rknn_sr(s);
        if (err != RKVC_OK)
            return err;
        rkvc_buffer *up = NULL;
        err = rkvc_rknn_sr_ctx_process(s->rknn_sr, host, &up);
        if (err != RKVC_OK)
            return err;
        *out = up;
        return RKVC_OK;
    }

    if (!s->rga_scale) {
        s->rga_scale = rkvc_rga_scale_ctx_create(s->desc.width, s->desc.height,
                                                 s->desc.post_upscale_algo);
        if (!s->rga_scale)
            return RKVC_ERR_HW;
    }

    rkvc_buffer *up = NULL;
    rkvc_err err = rkvc_rga_scale_ctx_process(s->rga_scale, host, &up);
    if (err != RKVC_OK)
        return err;
    *out = up;
    return RKVC_OK;
}

static int session_nv12_contiguous(const AVFrame *f)
{
    if (!f || !f->data[0] || f->format != AV_PIX_FMT_NV12 || !f->data[1])
        return 0;
    return (f->data[1] == f->data[0] + (ptrdiff_t)f->linesize[0] * f->height)
        && (f->linesize[1] == f->linesize[0]);
}

rkvc_err session_write_nv12_frame(FILE *fp, const AVFrame *f)
{
    const int h = f->height;
    const int ls = f->linesize[0];
    const int us = f->linesize[1];

    if (session_nv12_contiguous(f)) {
        const size_t nbytes = (size_t)ls * (size_t)h +
                             (size_t)us * (size_t)(h / 2);
        if (fwrite(f->data[0], 1, nbytes, fp) != nbytes)
            return RKVC_ERR_IO;
        return RKVC_OK;
    }

    for (int y = 0; y < h; y++)
        if (fwrite(f->data[0] + y * ls, 1, (size_t)ls, fp) != (size_t)ls)
            return RKVC_ERR_IO;
    for (int y = 0; y < h / 2; y++)
        if (fwrite(f->data[1] + y * us, 1, (size_t)us, fp) != (size_t)us)
            return RKVC_ERR_IO;
    return RKVC_OK;
}

rkvc_err session_write_nv12_buffer(FILE *fp, const rkvc_buffer *buf)
{
    if (!buf || !buf->av_frame)
        return RKVC_ERR_INVALID;

    const AVFrame *f = buf->av_frame;
    rkvc_err err = rkvc_buffer_dmabuf_begin_cpu_read(buf);
    if (err != RKVC_OK)
        return err;

    err = session_write_nv12_frame(fp, f);
    rkvc_buffer_dmabuf_end_cpu_read(buf);
    return err;
}
