/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session_run.c
 * @brief Session 文件/Live 循环：编解码、flush、端口输出。
 */

#include "internal.h"
#include "session_priv.h"

static double session_now_sec(void)
{
    return (double)rkvc_now_us() * 1e-6;
}

static rkvc_err session_push_output(rkvc_session *s, rkvc_buffer *buf)
{
    rkvc_err perr = rkvc_port_push(&s->port_output, buf);
    if (perr == RKVC_OK) {
        pthread_mutex_lock(&s->lock);
        s->stats.bytes_out += buf ? buf->size : 0;
        pthread_mutex_unlock(&s->lock);
        return RKVC_OK;
    }
    if (perr == RKVC_ERR_AGAIN) {
        rkvc_session_stats_drop(s);
        return RKVC_OK; /* 队列满：丢包记账，不中断管线 */
    }
    RKVC_LOG("output port push failed: %s", rkvc_err_str(perr));
    return perr;
}

static rkvc_err session_receive_packet(rkvc_session *s, rkvc_buffer **pkt)
{
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_enc)
        return rkvc_mlvc_enc_receive_packet(s->mlvc_enc, pkt);
#endif
    if (s->enc)
        return rkvc_mpp_enc_receive_packet(s->enc, pkt);
    return rkvc_svt_enc_receive_packet(s->svt, pkt);
}

static rkvc_err decode_pump_read_packet(void *opaque, rkvc_buffer **pkt)
{
    rkvc_session *s = opaque;
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_demux)
        return rkvc_mlvc_demux_read_packet(s->mlvc_demux, pkt);
#endif
    return rkvc_demux_read_packet(s->demux, pkt);
}

static rkvc_err decode_pump_send_packet(void *opaque,
                                        const rkvc_buffer *pkt)
{
    rkvc_session *s = opaque;
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_dec)
        return rkvc_mlvc_dec_send_packet(s->mlvc_dec, pkt);
#endif
    return rkvc_mpp_dec_send_packet(s->dec, pkt);
}

static rkvc_err decode_pump_receive_frame(void *opaque, rkvc_buffer **frame)
{
    rkvc_session *s = opaque;
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_dec)
        return rkvc_mlvc_dec_receive_frame(s->mlvc_dec, frame);
#endif
    return rkvc_mpp_dec_receive_frame(s->dec, frame);
}

static rkvc_err decode_pump_drain(void *opaque)
{
    rkvc_session *s = opaque;
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_dec)
        return rkvc_mlvc_dec_send_packet(s->mlvc_dec, NULL);
#endif
    return rkvc_mpp_dec_drain(s->dec);
}

static const rkvc_decode_pump_ops session_decode_pump_ops = {
    .read_packet   = decode_pump_read_packet,
    .send_packet   = decode_pump_send_packet,
    .receive_frame = decode_pump_receive_frame,
    .drain         = decode_pump_drain,
};

rkvc_err rkvc_decode_pump_next(rkvc_decode_pump *pump,
                               const rkvc_decode_pump_ops *ops,
                               void *opaque,
                               rkvc_buffer **frame)
{
    if (!pump || !ops || !ops->read_packet || !ops->send_packet ||
        !ops->receive_frame || !ops->drain || !frame)
        return RKVC_ERR_INVALID;

    *frame = NULL;

    if (!pump->pending_pkt && !pump->input_eof) {
        rkvc_err err = ops->read_packet(opaque, &pump->pending_pkt);
        if (err == RKVC_ERR_EOF) {
            pump->input_eof = 1;
        } else if (err != RKVC_OK) {
            return err;
        } else if (!pump->pending_pkt) {
            return RKVC_ERR_INTERNAL;
        }
    }

    int send_again = 0;
    if (pump->pending_pkt) {
        rkvc_err err = ops->send_packet(opaque, pump->pending_pkt);
        if (err == RKVC_OK) {
            rkvc_buffer_unref(pump->pending_pkt);
            pump->pending_pkt = NULL;
        } else if (err == RKVC_ERR_AGAIN) {
            send_again = 1;
        } else {
            return err;
        }
    }

    int drain_again = 0;
    if (pump->input_eof && !pump->pending_pkt && !pump->drain_sent) {
        rkvc_err err = ops->drain(opaque);
        if (err == RKVC_OK) {
            pump->drain_sent = 1;
        } else if (err == RKVC_ERR_AGAIN) {
            drain_again = 1;
        } else {
            return err;
        }
    }

    rkvc_err err = ops->receive_frame(opaque, frame);
    if (err == RKVC_OK)
        return *frame ? RKVC_OK : RKVC_ERR_INTERNAL;
    if (err == RKVC_ERR_EOF) {
        if (pump->drain_sent)
            return RKVC_ERR_EOF;
        rkvc_log_print(AV_LOG_ERROR,
                       "decoder returned EOF before drain was accepted\n");
        return RKVC_ERR_INTERNAL;
    }
    if (err != RKVC_ERR_AGAIN)
        return err;

    /* rkmpp is asynchronous and may transiently return EAGAIN from both ends.
       Keep the pending input/drain state and avoid a tight retry loop. */
    if (send_again || drain_again || pump->drain_sent)
        av_usleep(100);
    return RKVC_ERR_AGAIN;
}

void rkvc_decode_pump_cleanup(rkvc_decode_pump *pump)
{
    if (!pump)
        return;
    rkvc_buffer_unref(pump->pending_pkt);
    memset(pump, 0, sizeof(*pump));
}

static rkvc_err drain_encoder_packets(rkvc_session *s);

static rkvc_err session_write_packet(rkvc_session *s, const rkvc_buffer *pkt)
{
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_mux)
        return rkvc_mlvc_mux_write_packet(s->mlvc_mux, pkt);
#endif
    return rkvc_mux_write_packet(s->mux, pkt);
}

static rkvc_err session_drain_encoder(rkvc_session *s)
{
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_enc)
        return rkvc_mlvc_enc_drain(s->mlvc_enc);
#endif
    return s->enc ? rkvc_mpp_enc_drain(s->enc)
                  : rkvc_svt_enc_drain(s->svt);
}

static rkvc_err session_flush_encoder(rkvc_session *s)
{
    for (;;) {
        rkvc_err err = session_drain_encoder(s);
        if (err == RKVC_OK)
            break;
        if (err != RKVC_ERR_AGAIN)
            return err;
        err = drain_encoder_packets(s);
        if (err != RKVC_OK)
            return err;
        av_usleep(100);
    }

    /* Flush 协议（FFmpeg avcodec / libsvtav1）：send_frame(NULL) 后须持续
       receive_packet 直至真正 EOF。libsvtav1 的 eb_receive_packet 在收尾阶段对空
       SVT 输出队列返回 EAGAIN（EB_NoErrorEmptyQueue）——此时 EOS 包尚未产出，若遇
       EAGAIN 即 break 会丢尾包（preset 11 实测 90 帧入出帧不稳，39~54 抖动）。
       故 EAGAIN 时退让重试，收到有效包后继续，直至 EOF。 */
    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err err = session_receive_packet(s, &pkt);
        if (err == RKVC_ERR_EOF)
            break;
        if (err == RKVC_ERR_AGAIN) {
            av_usleep(100);
            continue;
        }
        if (err != RKVC_OK)
            return err;
        rkvc_err werr = session_write_packet(s, pkt);
        rkvc_err perr = session_push_output(s, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
        if (werr != RKVC_OK)
            return werr;
        if (perr != RKVC_OK)
            return perr;
    }
    return RKVC_OK;
}

static rkvc_err drain_encoder_packets(rkvc_session *s)
{
    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err err = session_receive_packet(s, &pkt);

        if (err == RKVC_ERR_AGAIN)
            return RKVC_OK;
        if (err == RKVC_ERR_EOF)
            return RKVC_OK;
        if (err != RKVC_OK)
            return err;

        rkvc_err werr = session_write_packet(s, pkt);
        rkvc_err perr = session_push_output(s, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
        if (werr != RKVC_OK)
            return werr;
        if (perr != RKVC_OK)
            return perr;
    }
}

static rkvc_err encode_one_frame(rkvc_session *s, rkvc_buffer *frame)
{
    /* MPP 硬 ROI：session ROI → AVFrame side data → rkmppenc → KEY_ROI_DATA。
     * SVT 无硬 ROI，忽略 set_roi（不做像素 fallback）。 */
    rkvc_roi_rect rois[RKVC_ROI_MAX];
    int roi_n = 0;
    int force_idr = 0;

    rkvc_err rerr = rkvc_session_apply_reconfig(s, &force_idr);
    if (rerr != RKVC_OK)
        return rerr;

    pthread_mutex_lock(&s->lock);
    roi_n = s->roi_count;
    if (roi_n > 0)
        memcpy(rois, s->rois, (size_t)roi_n * sizeof(rois[0]));
    pthread_mutex_unlock(&s->lock);

    for (;;) {
        rkvc_err err;
#ifdef RKVC_ENABLE_MLVC
        if (s->mlvc_enc) {
            err = rkvc_mlvc_enc_send_frame(s->mlvc_enc, frame);
        } else
#endif
        if (s->enc) {
            err = rkvc_mpp_enc_send_frame_roi_ex(
                s->enc, frame, roi_n > 0 ? rois : NULL, roi_n, force_idr);
        } else {
            err = rkvc_svt_enc_send_frame(s->svt, frame);
        }
        if (err == RKVC_OK)
            return drain_encoder_packets(s);
        if (err == RKVC_ERR_AGAIN) {
            err = drain_encoder_packets(s);
            if (err != RKVC_OK)
                return err;
            av_usleep(100);
            continue;
        }
        return err;
    }
}

static rkvc_err transcode_loop(rkvc_session *s)
{
    rkvc_err err = RKVC_OK;
    rkvc_decode_pump pump = {0};

    while (!s->stop_requested) {
        rkvc_buffer *frame = NULL;
        err = rkvc_decode_pump_next(&pump, &session_decode_pump_ops, s, &frame);
        if (err == RKVC_ERR_AGAIN) {
            if (s->svt) {
                err = drain_encoder_packets(s);
                if (err != RKVC_OK)
                    break;
            }
            continue;
        }
        if (err == RKVC_ERR_EOF) {
            err = RKVC_OK;
            break;
        }
        if (err != RKVC_OK)
            break;

        rkvc_session_stats_frame_in(s);

        rkvc_buffer *enc_frame = NULL;
        err = session_downscale_for_encode(s, frame, &enc_frame);
        if (err != RKVC_OK) {
            rkvc_buffer_unref(frame);
            break;
        }

        err = encode_one_frame(s, enc_frame);
        if (enc_frame != frame)
            rkvc_buffer_unref(enc_frame);
        rkvc_buffer_unref(frame);
        if (err != RKVC_OK)
            break;
    }

    rkvc_decode_pump_cleanup(&pump);
    if (err != RKVC_OK && err != RKVC_ERR_EOF)
        return err;
    return session_flush_encoder(s);
}

static rkvc_err session_write_display(rkvc_session *s, FILE *fp,
                                      rkvc_buffer *display)
{
    if (!display || !display->av_frame)
        return RKVC_OK;

    rkvc_buffer *write_buf = display;
    rkvc_buffer *cpu = NULL;
    rkvc_err err = RKVC_OK;

    if (display->mem_type == RKVC_MEM_DMABUF &&
        (!display->av_frame->data[0] ||
         display->av_frame->format == AV_PIX_FMT_DRM_PRIME)) {
        err = rkvc_dma_to_host(display, &cpu);
        if (err == RKVC_OK)
            write_buf = cpu;
    }
    if (err == RKVC_OK && write_buf->av_frame) {
        if (write_buf->mem_type == RKVC_MEM_DMABUF)
            err = session_write_nv12_buffer(fp, write_buf);
        else
            err = session_write_nv12_frame(fp, write_buf->av_frame);
    }
    rkvc_buffer_unref(cpu);
    if (err == RKVC_OK) {
        err = session_push_output(s, display);
        if (err == RKVC_OK)
            rkvc_session_stats_tick(s, 1);
    }
    return err;
}

static rkvc_err session_decode_one_frame(rkvc_session *s,
                                         rkvc_decode_pump *pump,
                                         rkvc_buffer **frame_out,
                                         double *decode_sec)
{
    const double t0 = session_now_sec();
    rkvc_err err = RKVC_OK;

    rkvc_buffer *frame = NULL;
    err = rkvc_decode_pump_next(pump, &session_decode_pump_ops, s, &frame);
    if (err == RKVC_ERR_AGAIN) {
        *decode_sec = session_now_sec() - t0;
        return RKVC_ERR_AGAIN;
    }
    if (err == RKVC_ERR_EOF || err != RKVC_OK) {
        *decode_sec = session_now_sec() - t0;
        return err;
    }

    rkvc_buffer *host = NULL;
    err = session_frame_to_host(s, frame, &host);
    *decode_sec = session_now_sec() - t0;
    rkvc_buffer_unref(frame);
    if (err != RKVC_OK) {
        rkvc_buffer_unref(host);
        return err;
    }

    *frame_out = host;
    return RKVC_OK;
}

static rkvc_err decode_loop_ai_sr(rkvc_session *s, FILE *fp)
{
    rkvc_err err = session_ensure_rknn_sr(s);
    if (err != RKVC_OK)
        return err;

    rkvc_decode_pump pump = {0};
    int has_pending = 0;

    while (!s->stop_requested) {
        rkvc_buffer *host = NULL;
        double decode_sec = 0.0;
        err = session_decode_one_frame(s, &pump, &host, &decode_sec);
        if (err == RKVC_ERR_AGAIN) {
            continue;
        }
        if (err == RKVC_ERR_EOF) {
            err = RKVC_OK;
            break;
        }
        if (err != RKVC_OK)
            goto out;

        rkvc_session_stats_add_timing(s, decode_sec, 0, 0, 0);

        if (has_pending) {
            rkvc_buffer *display = NULL;
            const double t_rga0 = session_now_sec();
            err = rkvc_rknn_sr_ctx_collect(s->rknn_sr, &display, 1);
            const double t_rga1 = session_now_sec();
            double wr_delta = 0.0;
            double pp_delta = 0.0;
            if (err == RKVC_OK) {
                const double t_wr0 = session_now_sec();
                err = session_write_display(s, fp, display);
                const double t_wr1 = session_now_sec();
                wr_delta = t_wr1 - t_wr0;
                pp_delta = t_wr1 - t_rga0;
            }
            rkvc_session_stats_add_timing(s, 0, t_rga1 - t_rga0, wr_delta, pp_delta);
            rkvc_buffer_unref(display);
            if (err != RKVC_OK) {
                rkvc_buffer_unref(host);
                goto out;
            }
            has_pending = 0;
        }

        const double t_rga0 = session_now_sec();
        err = rkvc_rknn_sr_ctx_submit(s->rknn_sr, host);
        const double t_rga1 = session_now_sec();
        rkvc_session_stats_add_timing(s, 0, t_rga1 - t_rga0, 0, 0);
        rkvc_buffer_unref(host);
        if (err != RKVC_OK)
            goto out;

        has_pending = 1;
    }

    if (has_pending) {
        rkvc_buffer *display = NULL;
        const double t_rga0 = session_now_sec();
        err = rkvc_rknn_sr_ctx_collect(s->rknn_sr, &display, 1);
        const double t_rga1 = session_now_sec();
        double wr_delta = 0.0;
        double pp_delta = 0.0;
        if (err == RKVC_OK) {
            const double t_wr0 = session_now_sec();
            err = session_write_display(s, fp, display);
            const double t_wr1 = session_now_sec();
            wr_delta = t_wr1 - t_wr0;
            pp_delta = t_wr1 - t_rga0;
        }
        rkvc_session_stats_add_timing(s, 0, t_rga1 - t_rga0, wr_delta, pp_delta);
        rkvc_buffer_unref(display);
    }

out:
    rkvc_decode_pump_cleanup(&pump);
    return err;
}

static rkvc_err decode_loop(rkvc_session *s)
{
    if (!s->desc.output_path)
        return RKVC_ERR_INVALID;

    FILE *fp = fopen(s->desc.output_path, "wb");
    if (!fp)
        return RKVC_ERR_IO;

    rkvc_err err = RKVC_OK;

    rkvc_session_stats_reset_timing(s);

    if (session_needs_post_upscale(s) && session_uses_ai_sr(s)) {
        err = decode_loop_ai_sr(s, fp);
        fclose(fp);
        return err;
    }

    rkvc_decode_pump pump = {0};
    while (!s->stop_requested) {
        const double t_dec0 = session_now_sec();
        rkvc_buffer *frame = NULL;
        err = rkvc_decode_pump_next(&pump, &session_decode_pump_ops, s,
                                    &frame);
        if (err == RKVC_ERR_AGAIN)
            continue;
        if (err == RKVC_ERR_EOF) {
            err = RKVC_OK;
            break;
        }
        if (err != RKVC_OK)
            break;

        rkvc_buffer *host = NULL;
        err = session_frame_to_host(s, frame, &host);
        const double t_dec1 = session_now_sec();

        rkvc_buffer *display = NULL;
        const double t_rga0 = session_now_sec();
        if (err == RKVC_OK && host)
            err = session_apply_post_upscale(s, host, &display);
        const double t_rga1 = session_now_sec();

        double t_wr1 = t_rga1;
        double wr_delta = 0.0;
        if (err == RKVC_OK && display && display->av_frame) {
            const double t_wr0 = session_now_sec();
            err = session_write_display(s, fp, display);
            t_wr1 = session_now_sec();
            wr_delta = t_wr1 - t_wr0;
        }

        rkvc_session_stats_add_timing(s, t_dec1 - t_dec0,
                                      t_rga1 - t_rga0, wr_delta,
                                      t_wr1 - t_rga0);

        if (display != host)
            rkvc_buffer_unref(display);
        rkvc_buffer_unref(host);
        rkvc_buffer_unref(frame);

        if (err != RKVC_OK)
            break;
    }

    rkvc_decode_pump_cleanup(&pump);
    fclose(fp);
    return err;
}

static rkvc_err load_raw_frame(FILE *fp, rkvc_buffer *frame, int w, int h,
                               rkvc_pix_fmt fmt)
{
    const size_t y_size = (size_t)w * (size_t)h;
    size_t frame_bytes = y_size + y_size / 2;
    uint8_t *raw = rkvc_malloc(frame_bytes);
    if (!raw)
        return RKVC_ERR_NOMEM;

    if (fread(raw, 1, frame_bytes, fp) != frame_bytes) {
        rkvc_free(raw);
        return RKVC_ERR_EOF;
    }

    uint8_t *planes[4] = {0};
    int strides[4] = {0};
    rkvc_buffer_get_video_planes(frame, planes, strides);

    for (int y = 0; y < h; y++)
        memcpy(planes[0] + y * strides[0],
               raw + (size_t)y * (size_t)w, (size_t)w);

    if (fmt == RKVC_PIX_FMT_YUV420P) {
        const size_t c_w = (size_t)w / 2;
        const size_t c_h = (size_t)h / 2;
        const uint8_t *u_src = raw + y_size;
        const uint8_t *v_src = u_src + c_w * c_h;
        for (int y = 0; y < (int)c_h; y++) {
            memcpy(planes[1] + y * strides[1], u_src + y * c_w, c_w);
            memcpy(planes[2] + y * strides[2], v_src + y * c_w, c_w);
        }
    } else {
        for (int y = 0; y < h / 2; y++)
            memcpy(planes[1] + y * strides[1],
                   raw + y_size + (size_t)y * (size_t)w, (size_t)w);
    }

    rkvc_free(raw);
    return RKVC_OK;
}

static rkvc_err live_capture_loop(rkvc_session *s)
{
    if (!s->v4l2)
        return RKVC_ERR_INVALID;

    rkvc_err err = RKVC_OK;
    int frames = 0;
    const int max_frames = s->desc.capture_max_frames;
    const int timeout_ms = s->desc.capture_timeout_ms > 0
                               ? s->desc.capture_timeout_ms
                               : 1000;

    while (!s->stop_requested) {
        if (max_frames > 0 && frames >= max_frames)
            break;

        rkvc_buffer *frame = NULL;
        err = rkvc_v4l2_read_frame(s->v4l2, &frame, timeout_ms);
        if (err == RKVC_ERR_AGAIN)
            continue;
        if (err != RKVC_OK)
            return err;

        if (rkvc_port_push(&s->port_capture, frame) == RKVC_ERR_AGAIN) {
            rkvc_buffer *drop = NULL;
            if (rkvc_port_pull(&s->port_capture, &drop, 0) == RKVC_OK)
                rkvc_buffer_unref(drop);
            (void)rkvc_port_push(&s->port_capture, frame);
        }
        /* preview：同帧侧抽，满则丢最旧（低延迟预览） */
        if (rkvc_port_push(&s->port_preview, frame) == RKVC_ERR_AGAIN) {
            rkvc_buffer *drop = NULL;
            if (rkvc_port_pull(&s->port_preview, &drop, 0) == RKVC_OK)
                rkvc_buffer_unref(drop);
            (void)rkvc_port_push(&s->port_preview, frame);
        }
        rkvc_session_stats_frame_in(s);
        frames++;

        rkvc_buffer *enc_frame = NULL;
        err = session_downscale_for_encode(s, frame, &enc_frame);
        if (err != RKVC_OK) {
            rkvc_buffer_unref(frame);
            return err;
        }

        err = encode_one_frame(s, enc_frame);
        if (enc_frame != frame)
            rkvc_buffer_unref(enc_frame);
        rkvc_buffer_unref(frame);
        if (err != RKVC_OK)
            return err;
    }

    return session_flush_encoder(s);
}

static rkvc_err encode_file_loop(rkvc_session *s)
{
    if (!s->desc.input_path)
        return RKVC_ERR_INVALID;

    FILE *fp = fopen(s->desc.input_path, "rb");
    if (!fp)
        return RKVC_ERR_IO;

    const int w = s->desc.width;
    const int h = s->desc.height;
    rkvc_err err = RKVC_OK;
    rkvc_err loop_err = RKVC_OK;
    int64_t pts = 0;

    while (!s->stop_requested) {
        rkvc_buffer *frame = NULL;
        err = rkvc_buffer_alloc_video_host(&frame, w, h, s->desc.pixel_format);
        if (err != RKVC_OK)
            break;

        err = load_raw_frame(fp, frame, w, h, s->desc.pixel_format);
        if (err == RKVC_ERR_EOF) {
            rkvc_buffer_unref(frame);
            err = RKVC_OK;
            break;
        }
        if (err != RKVC_OK) {
            rkvc_buffer_unref(frame);
            break;
        }

        rkvc_buffer_set_pts(frame, pts++);
        rkvc_session_stats_frame_in(s);

        rkvc_buffer *enc_frame = NULL;
        err = session_downscale_for_encode(s, frame, &enc_frame);
        if (err != RKVC_OK) {
            rkvc_buffer_unref(frame);
            break;
        }

        err = encode_one_frame(s, enc_frame);
        if (enc_frame != frame)
            rkvc_buffer_unref(enc_frame);
        rkvc_buffer_unref(frame);
        if (err != RKVC_OK) {
            loop_err = err;
            break;
        }
    }

    fclose(fp);

    if (loop_err != RKVC_OK)
        return loop_err;

    return session_flush_encoder(s);
}

rkvc_err rkvc_session_run_file(rkvc_session *session)
{
    if (!session)
        return RKVC_ERR_INVALID;

    rkvc_err err = rkvc_session_start(session);
    if (err != RKVC_OK)
        return err;

    switch (session->desc.template_id) {
    case RKVC_TEMPLATE_FILE_TRANSCODE:
    case RKVC_TEMPLATE_AV1_STORAGE:
    case RKVC_TEMPLATE_MLVC_STORAGE:
        err = transcode_loop(session);
        break;
    case RKVC_TEMPLATE_LIVE_CAPTURE:
        err = live_capture_loop(session);
        break;
    case RKVC_TEMPLATE_FILE_DECODE:
        err = decode_loop(session);
        break;
    case RKVC_TEMPLATE_FILE_ENCODE:
        err = encode_file_loop(session);
        break;
    default:
        err = RKVC_ERR_INVALID;
    }

    rkvc_session_stop(session);
    return err;
}
