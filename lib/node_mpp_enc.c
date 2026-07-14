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
    if (herr != RKVC_OK)
        rkvc_log_print(AV_LOG_WARNING,
                       "mpp_enc: RKMPP hw device init failed (err=%d), "
                       "encoder open may fail next\n", herr);
    if (hw)
        av_buffer_unref(&hw);

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


rkvc_err rkvc_mpp_enc_send_frame(rkvc_mpp_enc *enc, rkvc_buffer *frame)
{
    return rkvc_mpp_enc_send_frame_roi_ex(enc, frame, NULL, 0, 0);
}

rkvc_err rkvc_mpp_enc_apply_rc(rkvc_mpp_enc *enc, int64_t bitrate, int gop_size)
{
    if (!enc || !enc->ctx)
        return RKVC_ERR_INVALID;
    if (bitrate > 0)
        enc->ctx->bit_rate = bitrate;
    if (gop_size >= 1)
        enc->ctx->gop_size = gop_size;
    return RKVC_OK;
}

rkvc_err rkvc_mpp_enc_send_frame_roi(rkvc_mpp_enc *enc, rkvc_buffer *frame,
                                     const rkvc_roi_rect *rois, int roi_count)
{
    return rkvc_mpp_enc_send_frame_roi_ex(enc, frame, rois, roi_count, 0);
}

rkvc_err rkvc_mpp_enc_send_frame_roi_ex(rkvc_mpp_enc *enc, rkvc_buffer *frame,
                                        const rkvc_roi_rect *rois, int roi_count,
                                        int force_idr)
{
    if (!enc || !enc->ctx)
        return RKVC_ERR_INVALID;
    if (enc->flushed)
        return RKVC_ERR_EOF;
    if (!frame)
        return rkvc_mpp_enc_drain(enc);

    AVFrame *avf = NULL;
    rkvc_err err = rkvc_buffer_to_host_frame(frame, &avf);
    if (err != RKVC_OK)
        return err;

    /* 解复用 PTS 与编码器 time_base 不同；统一用单调帧序号，避免 MP4 帧率错乱。 */
    avf->pts = enc->next_pts++;

    if (force_idr)
        avf->pict_type = AV_PICTURE_TYPE_I;

    if (rois && roi_count > 0) {
        int n = roi_count > RKVC_ROI_MAX ? RKVC_ROI_MAX : roi_count;
        size_t bytes = (size_t)n * sizeof(AVRegionOfInterest);
        AVFrameSideData *sd =
            av_frame_new_side_data(avf, AV_FRAME_DATA_REGIONS_OF_INTEREST,
                                   (int)bytes);
        if (!sd) {
            av_frame_free(&avf);
            return RKVC_ERR_NOMEM;
        }
        AVRegionOfInterest *dst = (AVRegionOfInterest *)sd->data;
        /* force_intra 不在 AVRegionOfInterest 内：用帧 metadata 传给 rkmppenc */
        char intra_buf[RKVC_ROI_MAX * 2 + 1];
        size_t ip = 0;
        for (int i = 0; i < n; i++) {
            int qp = rois[i].qp_offset;
            if (qp > 51)
                qp = 51;
            if (qp < -51)
                qp = -51;
            dst[i].self_size = sizeof(AVRegionOfInterest);
            dst[i].top = rois[i].y;
            dst[i].left = rois[i].x;
            dst[i].bottom = rois[i].y + rois[i].h;
            dst[i].right = rois[i].x + rois[i].w;
            /* 唯一换算点：整数 qp_offset → FFmpeg qoffset = qp/51 */
            dst[i].qoffset = (AVRational){ qp, 51 };

            if (ip + 2 < sizeof(intra_buf)) {
                if (i)
                    intra_buf[ip++] = ',';
                intra_buf[ip++] = rois[i].force_intra ? '1' : '0';
            }
        }
        intra_buf[ip] = '\0';
        if (av_dict_set(&avf->metadata, "rkvc_roi_force_intra", intra_buf, 0) < 0) {
            av_frame_free(&avf);
            return RKVC_ERR_NOMEM;
        }
    }

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

    rkvc_buffer *b = rkvc_buffer_from_avpacket(enc->pkt);
    if (!b)
        return RKVC_ERR_NOMEM;

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
