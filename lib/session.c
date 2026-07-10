/**
 * @file session.c
 * @brief rkvc v2 会话：图构建与文件/端口管线。
 */

#include "internal.h"

static double session_now_sec(void)
{
    return (double)rkvc_now_us() * 1e-6;
}

static int session_align2(int v)
{
    return v > 0 ? (v & ~1) : 0;
}

static void session_enc_size(const rkvc_session *s, int *ew, int *eh)
{
    int denom = s->desc.enc_scale_denom;
    if (denom <= 1) {
        *ew = s->desc.width;
        *eh = s->desc.height;
        return;
    }
    *ew = session_align2(s->desc.width / denom);
    *eh = session_align2(s->desc.height / denom);
}

static int session_needs_post_upscale(const rkvc_session *s)
{
    return s->desc.post_upscale_algo != RKVC_UPSCALE_NONE &&
           s->desc.enc_scale_denom > 1;
}

static rkvc_err session_downscale_for_encode(rkvc_session *s,
                                             rkvc_buffer *frame,
                                             rkvc_buffer **out)
{
    int ew = 0, eh = 0;
    session_enc_size(s, &ew, &eh);
    *out = frame;

    if (s->desc.enc_scale_denom <= 1)
        return RKVC_OK;

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

static int session_uses_ai_sr(const rkvc_session *s)
{
    return s->desc.post_upscale_algo == RKVC_UPSCALE_AI_SR;
}

static int session_can_rga_upscale_dmabuf(const rkvc_session *s,
                                         const rkvc_buffer *frame)
{
    return session_needs_post_upscale(s) &&
           !session_uses_ai_sr(s) &&
           rkvc_rga_available() &&
           frame->mem_type == RKVC_MEM_DMABUF &&
           frame->format == RKVC_PIX_FMT_NV12;
}

static int session_can_ai_sr_dmabuf(const rkvc_session *s,
                                    const rkvc_buffer *frame)
{
    return session_needs_post_upscale(s) &&
           session_uses_ai_sr(s) &&
           rkvc_rga_available() &&
           frame->mem_type == RKVC_MEM_DMABUF &&
           frame->format == RKVC_PIX_FMT_NV12;
}

static rkvc_err session_frame_to_host(const rkvc_session *s,
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

static rkvc_err session_ensure_rknn_sr(rkvc_session *s)
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

static rkvc_err session_apply_post_upscale(rkvc_session *s,
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

static void session_write_nv12_frame(FILE *fp, const AVFrame *f)
{
    const int h = f->height;
    const int ls = f->linesize[0];
    const int us = f->linesize[1];

    if (session_nv12_contiguous(f)) {
        const size_t nbytes = (size_t)ls * (size_t)h +
                              (size_t)us * (size_t)(h / 2);
        fwrite(f->data[0], 1, nbytes, fp);
        return;
    }

    for (int y = 0; y < h; y++)
        fwrite(f->data[0] + y * ls, 1, (size_t)ls, fp);
    for (int y = 0; y < h / 2; y++)
        fwrite(f->data[1] + y * us, 1, (size_t)us, fp);
}

static void session_write_nv12_buffer(FILE *fp, const rkvc_buffer *buf)
{
    if (!buf || !buf->av_frame)
        return;

    const AVFrame *f = buf->av_frame;
    rkvc_err err = rkvc_buffer_dmabuf_begin_cpu_read(buf);
    if (err != RKVC_OK)
        return;

    session_write_nv12_frame(fp, f);
    rkvc_buffer_dmabuf_end_cpu_read(buf);
}

static void port_init(rkvc_port *p, const char *name, int depth,
                      rkvc_session *s)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->queue   = rkvc_port_queue_create(depth);
    p->session = s;
}

static void session_close_nodes(rkvc_session *s)
{
    if (!s)
        return;
    if (s->mux) {
        rkvc_mux_close(s->mux);
        s->mux = NULL;
    }
    if (s->enc) {
        rkvc_mpp_enc_close(s->enc);
        s->enc = NULL;
    }
    if (s->svt) {
        rkvc_svt_enc_close(s->svt);
        s->svt = NULL;
    }
    if (s->dec) {
        rkvc_mpp_dec_close(s->dec);
        s->dec = NULL;
    }
    if (s->demux) {
        rkvc_demux_close(s->demux);
        s->demux = NULL;
    }
    if (s->v4l2) {
        rkvc_v4l2_close(s->v4l2);
        s->v4l2 = NULL;
    }
    if (s->rga_scale) {
        rkvc_rga_scale_ctx_destroy(s->rga_scale);
        s->rga_scale = NULL;
    }
    if (s->rknn_sr) {
        rkvc_rknn_sr_ctx_destroy(s->rknn_sr);
        s->rknn_sr = NULL;
    }
}

static rkvc_err session_open_nodes(rkvc_session *s)
{
    /* 允许 start 失败后重试：先关掉上次半开的节点，避免 FD/指针泄漏 */
    session_close_nodes(s);

    const rkvc_pipeline_desc *d = &s->desc;

    if (d->template_id == RKVC_TEMPLATE_LIVE_CAPTURE) {
        if (!d->capture_device || !d->capture_device[0] || !d->output_path)
            return RKVC_ERR_INVALID;

        rkvc_v4l2_config vc = {
            .device  = d->capture_device,
            .width   = d->width,
            .height  = d->height,
            .fps_num = d->fps_num,
            .fps_den = d->fps_den,
        };
        rkvc_err err = rkvc_v4l2_open(&s->v4l2, &vc);
        if (err != RKVC_OK)
            return err;

        int aw = 0, ah = 0;
        rkvc_v4l2_get_size(s->v4l2, &aw, &ah);
        if (aw > 0 && ah > 0) {
            s->desc.width = aw;
            s->desc.height = ah;
            s->desc.pixel_format = RKVC_PIX_FMT_NV12;
        }
    }

    if (d->template_id == RKVC_TEMPLATE_FILE_TRANSCODE ||
        d->template_id == RKVC_TEMPLATE_FILE_DECODE ||
        d->template_id == RKVC_TEMPLATE_AV1_STORAGE) {
        if (!d->input_path)
            return RKVC_ERR_INVALID;

        rkvc_demux_config dc = {
            .input_path  = d->input_path,
            .format_opts = d->codec_opts,
        };
        rkvc_err err = rkvc_demux_open(&s->demux, &dc);
        if (err != RKVC_OK)
            return err;

        AVCodecParameters *par = rkvc_demux_video_par(s->demux);
        rkvc_mpp_dec_config mdc = {
            .route         = &s->route,
            .output_format = d->pixel_format,
            .low_latency   = d->low_latency,
            .codec_opts    = d->codec_opts,
        };
        err = rkvc_mpp_dec_open(&s->dec, &mdc, par);
        if (err != RKVC_OK)
            return err;
    }

    if (d->template_id == RKVC_TEMPLATE_FILE_ENCODE) {
        if (!d->output_path)
            return RKVC_ERR_INVALID;
    }

    if (d->template_id == RKVC_TEMPLATE_FILE_TRANSCODE ||
        d->template_id == RKVC_TEMPLATE_FILE_ENCODE ||
        d->template_id == RKVC_TEMPLATE_LIVE_CAPTURE ||
        d->template_id == RKVC_TEMPLATE_AV1_STORAGE) {
        if (!d->output_path)
            return RKVC_ERR_INVALID;

        if (s->route.enc_backend == RKVC_ENC_BACKEND_MPP) {
            int ew = 0, eh = 0;
            session_enc_size(s, &ew, &eh);
            rkvc_mpp_enc_config ec = {
                .route        = &s->route,
                .width        = ew,
                .height       = eh,
                .fps_num      = d->fps_num,
                .fps_den      = d->fps_den,
                .bitrate      = d->bitrate,
                .input_format = d->pixel_format,
                .gop_size     = d->gop_size,
                .low_latency  = d->low_latency,
                .rc_mode      = d->rc_mode,
                .qp_init      = d->qp_init,
                .codec_opts   = d->codec_opts,
            };
            rkvc_err err = rkvc_mpp_enc_open(&s->enc, &ec);
            if (err != RKVC_OK)
                return err;
        } else {
            int ew = 0, eh = 0;
            session_enc_size(s, &ew, &eh);
            rkvc_svt_enc_config sc = {
                .width        = ew,
                .height       = eh,
                .fps_num      = d->fps_num,
                .fps_den      = d->fps_den,
                .bitrate      = d->bitrate,
                .input_format = d->pixel_format,
                .gop_size     = d->gop_size,
                .svt_preset   = s->route.svt_preset,
                .svt_lp       = d->svt_lp,
                .svt_rtc      = d->svt_rtc,
                .rc_mode      = d->rc_mode,
            };
            rkvc_err err = rkvc_svt_enc_open(&s->svt, &sc);
            if (err != RKVC_OK)
                return err;
        }

        int ew = 0, eh = 0;
        session_enc_size(s, &ew, &eh);

        rkvc_mux_config mc = {
            .output_path  = d->output_path,
            .route        = &s->route,
            .width        = ew,
            .height       = eh,
            .fps_num      = d->fps_num,
            .fps_den      = d->fps_den,
            .bitrate      = d->bitrate,
            .pixel_format = d->pixel_format,
        };

        AVCodecParameters *par = avcodec_parameters_alloc();
        if (!par)
            return RKVC_ERR_NOMEM;
        par->codec_type = AVMEDIA_TYPE_VIDEO;
        par->width  = ew;
        par->height = eh;
        par->bit_rate = d->bitrate;
        if (s->route.codec == RKVC_CODEC_HEVC)
            par->codec_id = AV_CODEC_ID_HEVC;
        else if (s->route.codec == RKVC_CODEC_AV1)
            par->codec_id = AV_CODEC_ID_AV1;
        else
            par->codec_id = AV_CODEC_ID_H264;

        if (s->svt)
            rkvc_svt_enc_write_header(s->svt, par);

        rkvc_err err = rkvc_mux_open(&s->mux, &mc, par);
        avcodec_parameters_free(&par);
        if (err != RKVC_OK)
            return err;
    }

    return RKVC_OK;
}

rkvc_err rkvc_session_create(const rkvc_pipeline_desc *desc,
                             rkvc_session **out)
{
    if (!desc || !out)
        return RKVC_ERR_INVALID;

    if (desc->post_upscale_algo == RKVC_UPSCALE_AI_SR &&
        (!desc->post_upscale_rkvc_model_path ||
         !desc->post_upscale_rkvc_model_path[0]))
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_init();

    int rt_flags = 0;
    rkvc_err rerr = rkvc_runtime_try_register(desc, &rt_flags);
    if (rerr != RKVC_OK)
        return rerr;

    rkvc_session *s = rkvc_calloc(1, sizeof(*s));
    if (!s) {
        rkvc_runtime_unregister(rt_flags);
        return RKVC_ERR_NOMEM;
    }

    s->desc = *desc;
    s->runtime_flags = rt_flags;
    if (s->desc.queue_depth <= 0)
        s->desc.queue_depth = RKVC_PORT_QUEUE_DEFAULT;
    if (s->desc.capture_timeout_ms <= 0)
        s->desc.capture_timeout_ms = 1000;

    pthread_mutex_init(&s->lock, NULL);

    rkvc_err err = rkvc_route_resolve(&s->desc, &s->route);
    if (err != RKVC_OK) {
        rkvc_session_destroy(s);
        return err;
    }

    s->pool = rkvc_buffer_pool_create();
    if (!s->pool) {
        rkvc_session_destroy(s);
        return RKVC_ERR_NOMEM;
    }

    port_init(&s->port_capture, "capture", s->desc.queue_depth, s);
    port_init(&s->port_output, "output", s->desc.queue_depth, s);
    port_init(&s->port_preview, "preview", s->desc.queue_depth, s);

    s->stats.route = s->route;
    *out = s;
    return RKVC_OK;
}

rkvc_err rkvc_session_start(rkvc_session *session)
{
    if (!session)
        return RKVC_ERR_INVALID;
    if (session->running)
        return RKVC_OK;

    rkvc_err err = session_open_nodes(session);
    if (err != RKVC_OK) {
        session_close_nodes(session);
        return err;
    }

    session->running = 1;
    session->stats.running = 1;
    return RKVC_OK;
}

rkvc_err rkvc_session_stop(rkvc_session *session)
{
    if (!session)
        return RKVC_ERR_INVALID;
    session->stop_requested = 1;
    session->running = 0;
    session->stats.running = 0;
    return RKVC_OK;
}

rkvc_err rkvc_session_get_route(const rkvc_session *session,
                                rkvc_route_plan *plan)
{
    if (!session || !plan)
        return RKVC_ERR_INVALID;
    *plan = session->route;
    return RKVC_OK;
}

rkvc_port *rkvc_session_port(rkvc_session *session, const char *name)
{
    if (!session || !name)
        return NULL;
    if (strcmp(name, "capture") == 0)
        return &session->port_capture;
    if (strcmp(name, "output") == 0)
        return &session->port_output;
    if (strcmp(name, "preview") == 0)
        return &session->port_preview;
    return NULL;
}

rkvc_err rkvc_session_get_stats(const rkvc_session *session,
                                rkvc_session_stats *stats)
{
    if (!session || !stats)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock((pthread_mutex_t *)&session->lock);
    *stats = session->stats;
    pthread_mutex_unlock((pthread_mutex_t *)&session->lock);
    return RKVC_OK;
}

void rkvc_session_destroy(rkvc_session *session)
{
    if (!session)
        return;

    rkvc_session_stop(session);
    session_close_nodes(session);

    rkvc_port_queue_destroy(session->port_capture.queue);
    rkvc_port_queue_destroy(session->port_output.queue);
    rkvc_port_queue_destroy(session->port_preview.queue);

    rkvc_buffer_pool_destroy(session->pool);
    pthread_mutex_destroy(&session->lock);
    rkvc_runtime_unregister(session->runtime_flags);
    rkvc_free(session);
}

static rkvc_err drain_encoder_packets(rkvc_session *s)
{
    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err err;
        if (s->enc)
            err = rkvc_mpp_enc_receive_packet(s->enc, &pkt);
        else
            err = rkvc_svt_enc_receive_packet(s->svt, &pkt);

        if (err == RKVC_ERR_AGAIN)
            return RKVC_OK;
        if (err == RKVC_ERR_EOF)
            return RKVC_OK;
        if (err != RKVC_OK)
            return err;

        err = rkvc_mux_write_packet(s->mux, pkt);
        rkvc_port_push(&s->port_output, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
        if (err != RKVC_OK)
            return err;
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

    rkvc_err err;
    if (s->enc) {
        err = rkvc_mpp_enc_send_frame_roi_ex(s->enc, frame,
                                             roi_n > 0 ? rois : NULL, roi_n,
                                             force_idr);
        if (err != RKVC_OK && err != RKVC_ERR_AGAIN)
            return err;
        return drain_encoder_packets(s);
    }

    for (;;) {
        err = rkvc_svt_enc_send_frame(s->svt, frame);
        if (err == RKVC_OK)
            return drain_encoder_packets(s);
        if (err == RKVC_ERR_AGAIN) {
            err = drain_encoder_packets(s);
            if (err != RKVC_OK)
                return err;
            continue;
        }
        return err;
    }
}

static rkvc_err transcode_loop(rkvc_session *s)
{
    rkvc_err err = RKVC_OK;
    int dec_eof = 0;

    while (!s->stop_requested) {
        if (!dec_eof) {
            rkvc_buffer *pkt = NULL;
            err = rkvc_demux_read_packet(s->demux, &pkt);
            if (err == RKVC_ERR_EOF) {
                dec_eof = 1;
                rkvc_mpp_dec_drain(s->dec);
            } else if (err != RKVC_OK) {
                return err;
            } else {
                err = rkvc_mpp_dec_send_packet(s->dec, pkt);
                rkvc_buffer_unref(pkt);
                if (err != RKVC_OK && err != RKVC_ERR_AGAIN)
                    return err;
            }
        }

        rkvc_buffer *frame = NULL;
        err = rkvc_mpp_dec_receive_frame(s->dec, &frame);
        if (err == RKVC_ERR_AGAIN) {
            if (s->svt)
                drain_encoder_packets(s);
            continue;
        }
        if (err == RKVC_ERR_EOF)
            break;
        if (err != RKVC_OK)
            return err;

        rkvc_session_stats_frame_in(s);

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

    if (s->enc)
        rkvc_mpp_enc_drain(s->enc);
    else
        rkvc_svt_enc_drain(s->svt);

    for (;;) {
        rkvc_buffer *pkt = NULL;
        if (s->enc)
            err = rkvc_mpp_enc_receive_packet(s->enc, &pkt);
        else
            err = rkvc_svt_enc_receive_packet(s->svt, &pkt);
        if (err == RKVC_ERR_EOF)
            break;
        if (err == RKVC_ERR_AGAIN)
            continue;
        if (err != RKVC_OK)
            return err;
        rkvc_mux_write_packet(s->mux, pkt);
        rkvc_port_push(&s->port_output, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
    }

    return RKVC_OK;
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
            session_write_nv12_buffer(fp, write_buf);
        else
            session_write_nv12_frame(fp, write_buf->av_frame);
    }
    rkvc_buffer_unref(cpu);
    if (err == RKVC_OK) {
        rkvc_port_push(&s->port_output, display);
        rkvc_session_stats_tick(s, 1);
    }
    return err;
}

static rkvc_err session_decode_one_frame(rkvc_session *s, int *dec_eof,
                                         rkvc_buffer **frame_out,
                                         double *decode_sec)
{
    const double t0 = session_now_sec();
    rkvc_err err = RKVC_OK;

    if (!*dec_eof) {
        rkvc_buffer *pkt = NULL;
        err = rkvc_demux_read_packet(s->demux, &pkt);
        if (err == RKVC_ERR_EOF) {
            *dec_eof = 1;
            rkvc_mpp_dec_drain(s->dec);
        } else if (err != RKVC_OK) {
            return err;
        } else {
            err = rkvc_mpp_dec_send_packet(s->dec, pkt);
            rkvc_buffer_unref(pkt);
            if (err != RKVC_OK && err != RKVC_ERR_AGAIN)
                return err;
        }
    }

    rkvc_buffer *frame = NULL;
    err = rkvc_mpp_dec_receive_frame(s->dec, &frame);
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

    int dec_eof = 0;
    int has_pending = 0;

    while (!s->stop_requested) {
        rkvc_buffer *host = NULL;
        double decode_sec = 0.0;
        err = session_decode_one_frame(s, &dec_eof, &host, &decode_sec);
        if (err == RKVC_ERR_AGAIN) {
            if (dec_eof)
                break;
            continue;
        }
        if (err == RKVC_ERR_EOF)
            break;
        if (err != RKVC_OK)
            return err;

        s->stats.decode_sec += decode_sec;

        if (has_pending) {
            rkvc_buffer *display = NULL;
            const double t_rga0 = session_now_sec();
            err = rkvc_rknn_sr_ctx_collect(s->rknn_sr, &display, 1);
            const double t_rga1 = session_now_sec();
            s->stats.rga_sec += (t_rga1 - t_rga0);

            if (err == RKVC_OK) {
                const double t_wr0 = session_now_sec();
                err = session_write_display(s, fp, display);
                const double t_wr1 = session_now_sec();
                s->stats.write_sec += (t_wr1 - t_wr0);
                s->stats.postproc_sec += (t_wr1 - t_rga0);
            }
            rkvc_buffer_unref(display);
            if (err != RKVC_OK)
                return err;
            has_pending = 0;
        }

        const double t_rga0 = session_now_sec();
        err = rkvc_rknn_sr_ctx_submit(s->rknn_sr, host);
        const double t_rga1 = session_now_sec();
        s->stats.rga_sec += (t_rga1 - t_rga0);
        rkvc_buffer_unref(host);
        if (err != RKVC_OK)
            return err;

        has_pending = 1;
    }

    if (has_pending) {
        rkvc_buffer *display = NULL;
        const double t_rga0 = session_now_sec();
        err = rkvc_rknn_sr_ctx_collect(s->rknn_sr, &display, 1);
        const double t_rga1 = session_now_sec();
        s->stats.rga_sec += (t_rga1 - t_rga0);
        if (err == RKVC_OK) {
            const double t_wr0 = session_now_sec();
            err = session_write_display(s, fp, display);
            const double t_wr1 = session_now_sec();
            s->stats.write_sec += (t_wr1 - t_wr0);
            s->stats.postproc_sec += (t_wr1 - t_rga0);
        }
        rkvc_buffer_unref(display);
    }

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
    int dec_eof = 0;

    s->stats.decode_sec   = 0.0;
    s->stats.rga_sec      = 0.0;
    s->stats.write_sec    = 0.0;
    s->stats.postproc_sec = 0.0;

    if (session_needs_post_upscale(s) && session_uses_ai_sr(s)) {
        err = decode_loop_ai_sr(s, fp);
        fclose(fp);
        return err;
    }

    while (!s->stop_requested) {
        const double t_dec0 = session_now_sec();
        if (!dec_eof) {
            rkvc_buffer *pkt = NULL;
            err = rkvc_demux_read_packet(s->demux, &pkt);
            if (err == RKVC_ERR_EOF) {
                dec_eof = 1;
                rkvc_mpp_dec_drain(s->dec);
            } else if (err != RKVC_OK) {
                fclose(fp);
                return err;
            } else {
                err = rkvc_mpp_dec_send_packet(s->dec, pkt);
                rkvc_buffer_unref(pkt);
                if (err != RKVC_OK && err != RKVC_ERR_AGAIN) {
                    fclose(fp);
                    return err;
                }
            }
        }

        rkvc_buffer *frame = NULL;
        err = rkvc_mpp_dec_receive_frame(s->dec, &frame);
        if (err == RKVC_ERR_AGAIN) {
            if (dec_eof)
                break;
            continue;
        }
        if (err == RKVC_ERR_EOF)
            break;
        if (err != RKVC_OK) {
            fclose(fp);
            return err;
        }

        rkvc_buffer *host = NULL;
        err = session_frame_to_host(s, frame, &host);
        const double t_dec1 = session_now_sec();

        rkvc_buffer *display = NULL;
        const double t_rga0 = session_now_sec();
        if (err == RKVC_OK && host)
            err = session_apply_post_upscale(s, host, &display);
        const double t_rga1 = session_now_sec();

        double t_wr1 = t_rga1;
        if (err == RKVC_OK && display && display->av_frame) {
            const double t_wr0 = session_now_sec();
            err = session_write_display(s, fp, display);
            t_wr1 = session_now_sec();
            s->stats.write_sec += (t_wr1 - t_wr0);
        }

        s->stats.decode_sec   += (t_dec1 - t_dec0);
        s->stats.rga_sec      += (t_rga1 - t_rga0);
        s->stats.postproc_sec += (t_wr1 - t_rga0);

        if (display != host)
            rkvc_buffer_unref(display);
        rkvc_buffer_unref(host);
        rkvc_buffer_unref(frame);
    }

    fclose(fp);
    return RKVC_OK;
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

    if (s->enc)
        rkvc_mpp_enc_drain(s->enc);
    else if (s->svt)
        rkvc_svt_enc_drain(s->svt);

    for (;;) {
        rkvc_buffer *pkt = NULL;
        if (s->enc)
            err = rkvc_mpp_enc_receive_packet(s->enc, &pkt);
        else
            err = rkvc_svt_enc_receive_packet(s->svt, &pkt);
        if (err == RKVC_ERR_EOF)
            break;
        if (err == RKVC_ERR_AGAIN)
            continue;
        if (err != RKVC_OK)
            return err;
        rkvc_mux_write_packet(s->mux, pkt);
        rkvc_port_push(&s->port_output, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
    }

    return RKVC_OK;
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

    if (s->enc)
        rkvc_mpp_enc_drain(s->enc);
    else
        rkvc_svt_enc_drain(s->svt);

    for (;;) {
        rkvc_buffer *pkt = NULL;
        if (s->enc)
            err = rkvc_mpp_enc_receive_packet(s->enc, &pkt);
        else
            err = rkvc_svt_enc_receive_packet(s->svt, &pkt);
        if (err == RKVC_ERR_EOF)
            break;
        if (err == RKVC_ERR_AGAIN)
            continue;
        if (err != RKVC_OK)
            return err;
        rkvc_mux_write_packet(s->mux, pkt);
        rkvc_port_push(&s->port_output, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
    }

    return RKVC_OK;
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
