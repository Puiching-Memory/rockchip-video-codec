/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_svt_enc.c
 * @brief SVT-AV1 软件编码节点（经 FFmpeg libsvtav1）。
 *
 * 输入帧先落到 host AVFrame；libsvtav1 仅接受 YUV420P/YUV420P10，
 * NV12 等格式经 swscale 转换后再送入编码器。
 */

#include "internal.h"

struct rkvc_svt_enc {
    AVCodecContext *ctx;
    AVPacket       *pkt;
    struct SwsContext *sws;
    int             sws_src_fmt;
    int             width;
    int             height;
    int             flushed;
    int64_t         next_pts;
};


static rkvc_err ensure_yuv420p(rkvc_svt_enc *enc, AVFrame *src, AVFrame **out)
{
    if (!src)
        return RKVC_ERR_INVALID;

    if (src->format == AV_PIX_FMT_YUV420P) {
        *out = src;
        return RKVC_OK;
    }

    if (enc->sws && enc->sws_src_fmt != src->format) {
        sws_freeContext(enc->sws);
        enc->sws = NULL;
    }
    if (!enc->sws) {
        enc->sws = sws_getContext(enc->width, enc->height, src->format,
                                  enc->width, enc->height, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        if (!enc->sws)
            return RKVC_ERR_INTERNAL;
        enc->sws_src_fmt = src->format;
    }

    AVFrame *dst = av_frame_alloc();
    if (!dst)
        return RKVC_ERR_NOMEM;
    dst->format = AV_PIX_FMT_YUV420P;
    dst->width  = enc->width;
    dst->height = enc->height;
    if (av_frame_get_buffer(dst, 32) < 0) {
        av_frame_free(&dst);
        return RKVC_ERR_NOMEM;
    }
    if (sws_scale(enc->sws, (const uint8_t *const *)src->data, src->linesize,
                  0, enc->height, dst->data, dst->linesize) < 0) {
        av_frame_free(&dst);
        return RKVC_ERR_INTERNAL;
    }
    dst->pts = src->pts;
    av_frame_free(&src);
    *out = dst;
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_open(rkvc_svt_enc **out, const rkvc_svt_enc_config *cfg)
{
    if (!out || !cfg || cfg->width <= 0 || cfg->height <= 0)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_err init_err = rkvc_init();
    if (init_err != RKVC_OK)
        return init_err;

    const AVCodec *codec = avcodec_find_encoder_by_name("libsvtav1");
    if (!codec)
        return RKVC_ERR_NOT_FOUND;

    rkvc_svt_enc *enc = rkvc_calloc(1, sizeof(*enc));
    if (!enc)
        return RKVC_ERR_NOMEM;

    enc->width  = cfg->width;
    enc->height = cfg->height;
    enc->ctx    = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    enc->ctx->width     = cfg->width;
    enc->ctx->height    = cfg->height;
    enc->ctx->time_base = (AVRational){cfg->fps_den, cfg->fps_num};
    enc->ctx->framerate = (AVRational){cfg->fps_num, cfg->fps_den};
    enc->ctx->bit_rate  = cfg->bitrate;
    enc->ctx->gop_size  = cfg->gop_size;
    enc->ctx->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc->ctx->flags    |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* 不设 rc_max_rate==bit_rate，使 libsvtav1 走 VBR（含 CBR 请求）。 */
    enc->ctx->rc_max_rate = 0;

    if (!rkvc_is_valid_rc_mode(cfg->rc_mode)) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_INVALID;
    }

    AVDictionary *enc_opts = NULL;
    int preset = cfg->svt_preset > 0 ? cfg->svt_preset : RKVC_SVT_PRESET_PERF;
    rkvc_err perr = rkvc_dict_set_int(&enc_opts, "preset", preset);
    if (perr != RKVC_OK) {
        rkvc_svt_enc_close(enc);
        return perr;
    }

    /* 关闭 Dolby Vision 自动探测，避免无 RPU 时失败。 */
    if (av_dict_set(&enc_opts, "dolbyvision", "0", 0) < 0) {
        rkvc_dict_free(&enc_opts);
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    char params[64];
    int lp = cfg->svt_lp >= 0 ? cfg->svt_lp : RKVC_SVT_LP_AUTO;
    if (cfg->svt_rtc)
        snprintf(params, sizeof(params), "lp=%d:rtc=1", lp);
    else
        snprintf(params, sizeof(params), "lp=%d", lp);
    if (av_dict_set(&enc_opts, "svtav1-params", params, 0) < 0) {
        rkvc_dict_free(&enc_opts);
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    if (cfg->rc_mode == RKVC_RC_CQP) {
        /* 无独立 qp_init 时用中等 QP；可用后续 codec_opts 覆盖。 */
        perr = rkvc_dict_set_int(&enc_opts, "qp", 30);
        if (perr != RKVC_OK) {
            rkvc_dict_free(&enc_opts);
            rkvc_svt_enc_close(enc);
            return perr;
        }
    }

    perr = rkvc_opt_set_dict(enc->ctx->priv_data, &enc_opts);
    if (perr != RKVC_OK) {
        rkvc_dict_free(&enc_opts);
        rkvc_svt_enc_close(enc);
        return perr;
    }

    rkvc_err err = rkvc_codec_open2(enc->ctx, codec, &enc_opts, "svt_enc");
    rkvc_dict_free(&enc_opts);
    if (err != RKVC_OK) {
        rkvc_svt_enc_close(enc);
        return err;
    }

    enc->pkt = av_packet_alloc();
    if (!enc->pkt) {
        rkvc_svt_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    *out = enc;
    return RKVC_OK;
}

void rkvc_svt_enc_close(rkvc_svt_enc *enc)
{
    if (!enc)
        return;
    if (enc->sws)
        sws_freeContext(enc->sws);
    if (enc->pkt)
        av_packet_free(&enc->pkt);
    if (enc->ctx)
        avcodec_free_context(&enc->ctx);
    rkvc_free(enc);
}

rkvc_err rkvc_svt_enc_write_header(rkvc_svt_enc *enc, AVCodecParameters *par)
{
    if (!enc || !enc->ctx || !par)
        return RKVC_ERR_INVALID;

    if (enc->ctx->extradata && enc->ctx->extradata_size > 0) {
        av_freep(&par->extradata);
        par->extradata = av_mallocz((size_t)enc->ctx->extradata_size +
                                    AV_INPUT_BUFFER_PADDING_SIZE);
        if (!par->extradata)
            return RKVC_ERR_NOMEM;
        memcpy(par->extradata, enc->ctx->extradata,
               (size_t)enc->ctx->extradata_size);
        par->extradata_size = enc->ctx->extradata_size;
        return RKVC_OK;
    }

    /* GLOBAL_HEADER 已开但仍无 extradata 时，由 mux 侧容忍空头。 */
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_send_frame(rkvc_svt_enc *enc, rkvc_buffer *frame)
{
    if (!enc || !enc->ctx)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_ERR_EOF;
    if (!frame)
        return rkvc_svt_enc_drain(enc);

    AVFrame *avf = NULL;
    rkvc_err err = rkvc_buffer_to_host_frame(frame, &avf);
    if (err != RKVC_OK)
        return err;

    err = ensure_yuv420p(enc, avf, &avf);
    if (err != RKVC_OK) {
        av_frame_free(&avf);
        return err;
    }

    /* 解复用 PTS 与编码器 time_base 不同；统一用单调帧序号，避免 MP4 帧率错乱。 */
    avf->pts = enc->next_pts++;

    int ret = avcodec_send_frame(enc->ctx, avf);
    av_frame_free(&avf);
    if (ret == AVERROR(EAGAIN))
        return RKVC_ERR_AGAIN;
    return rkvc_from_averror(ret);
}

rkvc_err rkvc_svt_enc_receive_packet(rkvc_svt_enc *enc, rkvc_buffer **out)
{
    if (!enc || !enc->ctx || !out)
        return RKVC_ERR_INVALID;

    *out = NULL;
    av_packet_unref(enc->pkt);
    int ret = avcodec_receive_packet(enc->ctx, enc->pkt);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN))
            return RKVC_ERR_AGAIN;
        if (ret == AVERROR_EOF)
            return RKVC_ERR_EOF;
        return rkvc_from_averror(ret);
    }

    rkvc_buffer *b = rkvc_buffer_from_avpacket(enc->pkt);
    if (!b)
        return RKVC_ERR_NOMEM;

    *out = b;
    return RKVC_OK;
}

rkvc_err rkvc_svt_enc_drain(rkvc_svt_enc *enc)
{
    if (!enc || !enc->ctx)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_OK;
    enc->flushed = 1;
    int ret = avcodec_send_frame(enc->ctx, NULL);
    if (ret < 0 && ret != AVERROR_EOF)
        return rkvc_from_averror(ret);
    return RKVC_OK;
}
