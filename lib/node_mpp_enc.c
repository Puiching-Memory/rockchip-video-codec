/**
 * @file node_mpp_enc.c
 * @brief H.264/HEVC RKMPP 硬件编码。
 */

#include "internal.h"

struct rkvc_mpp_enc {
    AVCodecContext   *ctx;
    const rkvc_route_plan *route;
    AVPacket         *pkt;
    int               flushed;
    int64_t           next_pts;
};

rkvc_err rkvc_mpp_enc_open(rkvc_mpp_enc **out, const rkvc_mpp_enc_config *cfg)
{
    if (!out || !cfg || !cfg->route)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_init();

    const char *name = cfg->route->enc_name;
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
        if (cfg->route->codec == RKVC_CODEC_HEVC)
            codec = avcodec_find_encoder(AV_CODEC_ID_HEVC);
        else
            codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    }
    if (!codec)
        return RKVC_ERR_NOT_FOUND;

    rkvc_err perm = rkvc_check_hw_permissions();
    if (perm != RKVC_OK)
        return perm;

    rkvc_mpp_enc *enc = rkvc_calloc(1, sizeof(*enc));
    if (!enc)
        return RKVC_ERR_NOMEM;

    enc->route = cfg->route;
    enc->ctx   = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        rkvc_mpp_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    enc->ctx->width     = cfg->width;
    enc->ctx->height    = cfg->height;
    enc->ctx->time_base = (AVRational){cfg->fps_den, cfg->fps_num};
    enc->ctx->framerate = (AVRational){cfg->fps_num, cfg->fps_den};
    enc->ctx->bit_rate  = cfg->bitrate;
    enc->ctx->gop_size  = cfg->gop_size;
    enc->ctx->max_b_frames = 0;
    enc->ctx->thread_count = 1;

    if (cfg->low_latency)
        enc->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    enc->ctx->pix_fmt = rkvc_to_av_pix_fmt(cfg->input_format);

    if (!rkvc_is_valid_rc_mode(cfg->rc_mode)) {
        rkvc_mpp_enc_close(enc);
        return RKVC_ERR_INVALID;
    }

    AVDictionary *enc_opts = NULL;
    rkvc_err perr = rkvc_dict_parse_opts(&enc_opts, cfg->codec_opts);
    if (perr != RKVC_OK) {
        rkvc_mpp_enc_close(enc);
        return perr;
    }

    perr = rkvc_dict_set_int(&enc_opts, "rc_mode", cfg->rc_mode);
    if (perr != RKVC_OK) {
        rkvc_dict_free(&enc_opts);
        rkvc_mpp_enc_close(enc);
        return perr;
    }
    if (cfg->qp_init >= 0) {
        perr = rkvc_dict_set_int(&enc_opts, "qp_init", cfg->qp_init);
        if (perr != RKVC_OK) {
            rkvc_dict_free(&enc_opts);
            rkvc_mpp_enc_close(enc);
            return perr;
        }
    }

    perr = rkvc_opt_set_dict(enc->ctx->priv_data, &enc_opts);
    if (perr != RKVC_OK) {
        rkvc_dict_free(&enc_opts);
        rkvc_mpp_enc_close(enc);
        return perr;
    }

    AVBufferRef *hw = NULL;
    rkvc_err herr = rkvc_get_hw_device_ctx(&hw);
    if (hw)
        av_buffer_unref(&hw);
    (void)herr;

    rkvc_err err = rkvc_codec_open2(enc->ctx, codec, &enc_opts, "mpp_enc");
    rkvc_dict_free(&enc_opts);
    if (err != RKVC_OK) {
        rkvc_mpp_enc_close(enc);
        return err;
    }

    enc->pkt = av_packet_alloc();
    if (!enc->pkt) {
        rkvc_mpp_enc_close(enc);
        return RKVC_ERR_NOMEM;
    }

    *out = enc;
    return RKVC_OK;
}

void rkvc_mpp_enc_close(rkvc_mpp_enc *enc)
{
    if (!enc)
        return;
    if (enc->pkt)
        av_packet_free(&enc->pkt);
    if (enc->ctx)
        avcodec_free_context(&enc->ctx);
    rkvc_free(enc);
}

static rkvc_err host_frame_from_buffer(rkvc_buffer *buf, AVFrame **frame_out)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;

    if (buf->mem_type == RKVC_MEM_DMABUF) {
        rkvc_buffer *host = NULL;
        rkvc_err err = rkvc_dma_to_host(buf, &host);
        if (err != RKVC_OK)
            return err;
        *frame_out = av_frame_clone(host->av_frame);
        rkvc_buffer_unref(host);
        if (!*frame_out)
            return RKVC_ERR_NOMEM;
        return RKVC_OK;
    }

    if (!buf->av_frame)
        return RKVC_ERR_INVALID;

    *frame_out = av_frame_clone(buf->av_frame);
    if (!*frame_out)
        return RKVC_ERR_NOMEM;
    return RKVC_OK;
}

rkvc_err rkvc_mpp_enc_send_frame(rkvc_mpp_enc *enc, rkvc_buffer *frame)
{
    if (!enc || !enc->ctx)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_ERR_EOF;
    if (!frame)
        return rkvc_mpp_enc_drain(enc);

    AVFrame *avf = NULL;
    rkvc_err err = host_frame_from_buffer(frame, &avf);
    if (err != RKVC_OK)
        return err;

    if (avf->pts == AV_NOPTS_VALUE)
        avf->pts = enc->next_pts++;
    else
        enc->next_pts = avf->pts + 1;

    int ret = avcodec_send_frame(enc->ctx, avf);
    av_frame_free(&avf);
    if (ret == AVERROR(EAGAIN))
        return RKVC_ERR_AGAIN;
    return rkvc_from_averror(ret);
}

rkvc_err rkvc_mpp_enc_receive_packet(rkvc_mpp_enc *enc, rkvc_buffer **out)
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

    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b)
        return RKVC_ERR_NOMEM;

    b->kind = RKVC_BUF_BITSTREAM;
    pthread_mutex_init(&b->lock, NULL);
    b->ref_count = 1;
    b->data = rkvc_malloc((size_t)enc->pkt->size);
    if (!b->data) {
        rkvc_buffer_unref(b);
        return RKVC_ERR_NOMEM;
    }
    memcpy(b->data, enc->pkt->data, (size_t)enc->pkt->size);
    b->size      = (size_t)enc->pkt->size;
    b->owns_data = 1;
    b->pts       = enc->pkt->pts;
    b->dts       = enc->pkt->dts;
    b->key_frame = (enc->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

    *out = b;
    return RKVC_OK;
}

rkvc_err rkvc_mpp_enc_drain(rkvc_mpp_enc *enc)
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
