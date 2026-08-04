/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_mpp_dec.c
 * @brief RKMPP 硬件解码：热路径保留 DRM_PRIME/DMA-BUF。
 */

#include "internal.h"

struct rkvc_mpp_dec {
    AVCodecContext   *ctx;
    const rkvc_route_plan *route;
    rkvc_pix_fmt      output_format;
    int               flushed;
    struct SwsContext *sws;
    int               sws_src_w, sws_src_h, sws_src_fmt, sws_dst_fmt;
};

rkvc_err rkvc_mpp_dec_open(rkvc_mpp_dec **out, const rkvc_mpp_dec_config *cfg,
                           AVCodecParameters *par)
{
    if (!out || !cfg || !cfg->route || !par)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_err init_err = rkvc_init();
    if (init_err != RKVC_OK)
        return init_err;

    const char *name = cfg->route->dec_name;
    const AVCodec *codec = NULL;

    switch (par->codec_id) {
    case AV_CODEC_ID_H264:
        codec = avcodec_find_decoder_by_name("h264_rkmpp");
        break;
    case AV_CODEC_ID_HEVC:
        codec = avcodec_find_decoder_by_name("hevc_rkmpp");
        break;
    case AV_CODEC_ID_AV1:
        codec = avcodec_find_decoder_by_name("av1_rkmpp");
        break;
    default:
        break;
    }
    if (!codec)
        codec = avcodec_find_decoder_by_name(name);
    if (!codec) {
        if (cfg->route->codec == RKVC_CODEC_H264)
            codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        else if (cfg->route->codec == RKVC_CODEC_HEVC)
            codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
        else
            codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
    }
    if (!codec)
        return RKVC_ERR_NOT_FOUND;

    rkvc_err perm = rkvc_check_hw_permissions();
    if (perm != RKVC_OK)
        return perm;

    rkvc_mpp_dec *dec = rkvc_calloc(1, sizeof(*dec));
    if (!dec)
        return RKVC_ERR_NOMEM;

    dec->route         = cfg->route;
    dec->output_format = cfg->output_format;

    dec->ctx = avcodec_alloc_context3(codec);
    if (!dec->ctx) {
        rkvc_mpp_dec_close(dec);
        return RKVC_ERR_NOMEM;
    }

    int ret = avcodec_parameters_to_context(dec->ctx, par);
    if (ret < 0) {
        rkvc_mpp_dec_close(dec);
        return rkvc_from_averror(ret);
    }

    if (cfg->low_latency)
        dec->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    dec->ctx->thread_count = 1;

    AVDictionary *dec_opts = NULL;
    rkvc_err perr = rkvc_dict_parse_opts(&dec_opts, cfg->codec_opts);
    if (perr != RKVC_OK) {
        rkvc_mpp_dec_close(dec);
        return perr;
    }

    rkvc_err err = rkvc_codec_open2(dec->ctx, codec, &dec_opts, "mpp_dec");
    rkvc_dict_free(&dec_opts);
    if (err != RKVC_OK) {
        rkvc_mpp_dec_close(dec);
        return err;
    }

    *out = dec;
    return RKVC_OK;
}

void rkvc_mpp_dec_close(rkvc_mpp_dec *dec)
{
    if (!dec)
        return;
    if (dec->sws)
        sws_freeContext(dec->sws);
    if (dec->ctx)
        avcodec_free_context(&dec->ctx);
    rkvc_free(dec);
}

rkvc_err rkvc_mpp_dec_send_packet(rkvc_mpp_dec *dec, const rkvc_buffer *pkt)
{
    if (!dec || !dec->ctx)
        return RKVC_ERR_INVALID;
    if (dec->flushed)
        return RKVC_ERR_EOF;
    if (!pkt || pkt->kind != RKVC_BUF_BITSTREAM)
        return RKVC_ERR_INVALID;

    AVPacket *avpkt = av_packet_alloc();
    if (!avpkt)
        return RKVC_ERR_NOMEM;

    avpkt->data = pkt->data;
    avpkt->size = (int)pkt->size;
    avpkt->pts  = pkt->pts;
    avpkt->dts  = pkt->dts;

    int ret = avcodec_send_packet(dec->ctx, avpkt);
    rkvc_err err = (ret == AVERROR(EAGAIN)) ? RKVC_ERR_AGAIN : rkvc_from_averror(ret);
    av_packet_free(&avpkt);
    return err;
}

static rkvc_err dec_sws_to_format(rkvc_mpp_dec *dec, AVFrame *frame,
                                  rkvc_buffer **out)
{
    enum AVPixelFormat dst_av = rkvc_to_av_pix_fmt(dec->output_format);

    if (dec->sws &&
        (dec->sws_src_w != frame->width || dec->sws_src_h != frame->height ||
         dec->sws_src_fmt != frame->format || dec->sws_dst_fmt != dst_av)) {
        sws_freeContext(dec->sws);
        dec->sws = NULL;
    }

    if (!dec->sws) {
        dec->sws = sws_getContext(frame->width, frame->height, frame->format,
                                  frame->width, frame->height, dst_av,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        if (!dec->sws)
            return RKVC_ERR_FORMAT;
        dec->sws_src_w = frame->width;
        dec->sws_src_h = frame->height;
        dec->sws_src_fmt = frame->format;
        dec->sws_dst_fmt = dst_av;
    }

    AVFrame *dst = av_frame_alloc();
    if (!dst)
        return RKVC_ERR_NOMEM;
    dst->width  = frame->width;
    dst->height = frame->height;
    dst->format = dst_av;
    dst->pts    = frame->pts;

    int ret = av_frame_get_buffer(dst, 0);
    if (ret < 0) {
        av_frame_free(&dst);
        return rkvc_from_averror(ret);
    }

    ret = sws_scale(dec->sws,
                    (const uint8_t *const *)frame->data, frame->linesize,
                    0, frame->height, dst->data, dst->linesize);
    av_frame_free(&frame);
    if (ret <= 0) {
        av_frame_free(&dst);
        return RKVC_ERR_FORMAT;
    }

    *out = rkvc_buffer_wrap_avframe(dst, 1);
    if (!*out) {
        av_frame_free(&dst);
        return RKVC_ERR_NOMEM;
    }
    (*out)->format = dec->output_format;
    return RKVC_OK;
}

rkvc_err rkvc_mpp_dec_receive_frame(rkvc_mpp_dec *dec, rkvc_buffer **out)
{
    if (!dec || !dec->ctx || !out)
        return RKVC_ERR_INVALID;

    *out = NULL;
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return RKVC_ERR_NOMEM;

    int ret = avcodec_receive_frame(dec->ctx, frame);
    if (ret < 0) {
        av_frame_free(&frame);
        if (ret == AVERROR(EAGAIN))
            return RKVC_ERR_AGAIN;
        if (ret == AVERROR_EOF)
            return RKVC_ERR_EOF;
        return rkvc_from_averror(ret);
    }

    if (frame->format == AV_PIX_FMT_DRM_PRIME) {
        rkvc_buffer *dma = NULL;
        if (dec->output_format == RKVC_PIX_FMT_NV12)
            dma = rkvc_buffer_from_drm_frame(frame);
        if (dma) {
            dma->pts = frame->pts;
            *out = dma;
            return RKVC_OK;
        }
        rkvc_buffer_unref(dma);

        AVFrame *sw = av_frame_alloc();
        if (!sw) {
            av_frame_free(&frame);
            return RKVC_ERR_NOMEM;
        }
        sw->format = AV_PIX_FMT_NV12;
        ret = av_hwframe_transfer_data(sw, frame, 0);
        av_frame_free(&frame);
        if (ret < 0) {
            av_frame_free(&sw);
            return rkvc_from_averror(ret);
        }
        frame = sw;
    }

    enum AVPixelFormat want = rkvc_to_av_pix_fmt(dec->output_format);
    if (frame->format != want)
        return dec_sws_to_format(dec, frame, out);

    *out = rkvc_buffer_wrap_avframe(frame, 1);
    if (!*out) {
        av_frame_free(&frame);
        return RKVC_ERR_NOMEM;
    }
    (*out)->format = dec->output_format;
    return RKVC_OK;
}

rkvc_err rkvc_mpp_dec_drain(rkvc_mpp_dec *dec)
{
    if (!dec || !dec->ctx)
        return RKVC_ERR_INVALID;
    if (dec->flushed)
        return RKVC_OK;
    int ret = avcodec_send_packet(dec->ctx, NULL);
    dec->flushed = 1;
    if (ret < 0 && ret != AVERROR_EOF)
        return rkvc_from_averror(ret);
    return RKVC_OK;
}
