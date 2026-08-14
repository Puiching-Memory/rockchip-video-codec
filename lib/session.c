/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session.c
 * @brief rkvc 会话：图构建与文件/端口管线。
 */

#include "internal.h"
#ifdef RKVC_ENABLE_MLVC
#include "qppatch.h"
#endif

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

static rkvc_err session_write_nv12_frame(FILE *fp, const AVFrame *f)
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

static rkvc_err session_write_nv12_buffer(FILE *fp, const rkvc_buffer *buf)
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
#ifdef RKVC_ENABLE_MLVC
    if (s->mlvc_mux) {
        rkvc_mlvc_mux_close(s->mlvc_mux);
        s->mlvc_mux = NULL;
    }
    if (s->mlvc_enc) {
        rkvc_mlvc_enc_close(s->mlvc_enc);
        s->mlvc_enc = NULL;
    }
    if (s->mlvc_dec) {
        rkvc_mlvc_dec_close(s->mlvc_dec);
        s->mlvc_dec = NULL;
    }
    if (s->mlvc_demux) {
        rkvc_mlvc_demux_close(s->mlvc_demux);
        s->mlvc_demux = NULL;
    }
#endif
}

static int session_is_mlvc_file(const char *path)
{
    if (!path)
        return 0;
    size_t len = strlen(path);
    if (len < 5)
        return 0;
    return strcasecmp(path + len - 5, ".mlvc") == 0;
}

static int session_is_raw_out(const char *path)
{
    if (!path)
        return 0;
    size_t len = strlen(path);
    if (len >= 4 &&
        (strcasecmp(path + len - 4, ".yuv") == 0 ||
         strcasecmp(path + len - 4, ".raw") == 0))
        return 1;
    return 0;
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

    /* ── MLVC 输入侧：.mlvc 文件 → mlvc_demux + mlvc_dec ── */
#ifdef RKVC_ENABLE_MLVC
    int input_is_mlvc = (d->input_path &&
                         session_is_mlvc_file(d->input_path));
    if (input_is_mlvc &&
        (d->template_id == RKVC_TEMPLATE_FILE_DECODE ||
         d->template_id == RKVC_TEMPLATE_FILE_TRANSCODE ||
         d->template_id == RKVC_TEMPLATE_MLVC_STORAGE)) {
        if (!d->input_path)
            return RKVC_ERR_INVALID;
        rkvc_mlvc_demux_config mdc = { .input_path = d->input_path };
        rkvc_err err = rkvc_mlvc_demux_open(&s->mlvc_demux, &mdc);
        if (err != RKVC_OK)
            return err;
        int dw = 0, dh = 0;
        char dec_patch_buf[768];
        const char *dec_patch = NULL;
        rkvc_err perr = rkvc_qppatch_resolve(d->mlvc_qp_patch_dir, "dec",
                                             rkvc_mlvc_demux_qp(s->mlvc_demux),
                                             dec_patch_buf, sizeof dec_patch_buf,
                                             &dec_patch);
        if (perr != RKVC_OK)
            return perr;
        rkvc_mlvc_dec_config dcfg = {
            .dec_model_path    = d->mlvc_dec_model_path,
            .gaussian_pmf_path = d->mlvc_gaussian_pmf_path,
            .bitest_pmf_path   = d->mlvc_bitest_pmf_path,
            .qp                = rkvc_mlvc_demux_qp(s->mlvc_demux),
            .qp_patch_path     = dec_patch,
        };
        err = rkvc_mlvc_dec_open(&s->mlvc_dec, &dcfg);
        if (err != RKVC_OK)
            return err;
        dw = rkvc_mlvc_dec_width(s->mlvc_dec);
        dh = rkvc_mlvc_dec_height(s->mlvc_dec);
        if (dw > 0 && dh > 0) {
            s->desc.width  = dw;
            s->desc.height = dh;
        }
    }
#endif

    if (d->template_id == RKVC_TEMPLATE_FILE_TRANSCODE ||
        d->template_id == RKVC_TEMPLATE_FILE_DECODE ||
        d->template_id == RKVC_TEMPLATE_AV1_STORAGE ||
        d->template_id == RKVC_TEMPLATE_MLVC_STORAGE) {
        /* .mlvc 输入由上面 MLVC 分支处理 */
#ifdef RKVC_ENABLE_MLVC
        if (s->mlvc_demux)
            goto mlvc_input_done;
#endif
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
mlvc_input_done:
    ;

    if (d->template_id == RKVC_TEMPLATE_FILE_ENCODE) {
        if (!d->output_path)
            return RKVC_ERR_INVALID;
    }

    if (d->template_id == RKVC_TEMPLATE_FILE_TRANSCODE ||
        d->template_id == RKVC_TEMPLATE_FILE_ENCODE ||
        d->template_id == RKVC_TEMPLATE_LIVE_CAPTURE ||
        d->template_id == RKVC_TEMPLATE_AV1_STORAGE ||
        d->template_id == RKVC_TEMPLATE_MLVC_STORAGE) {
        if (!d->output_path)
            return RKVC_ERR_INVALID;

#ifdef RKVC_ENABLE_MLVC
        int output_is_mlvc = (d->output_path &&
                              session_is_mlvc_file(d->output_path));
        /* MLVC 编码器 + .mlvc 容器（不走 FFmpeg mux）。
         * 编解码器独立选择：解码器由输入决定（.mlvc 输入 → MLVC 解码），
         * 编码器由输出决定（.mlvc 输出 → MLVC 编码；标准容器 → 标准编码器）。
         * .mlvc → .yuv 纯解码不经过此输出块（FILE_DECODE 模板，无编码器）。 */
        if (s->route.enc_backend == RKVC_ENC_BACKEND_MLVC && output_is_mlvc) {
            if (!d->mlvc_enc_model_path ||
                !d->mlvc_gaussian_pmf_path || !d->mlvc_bitest_pmf_path)
                return RKVC_ERR_INVALID;
            char enc_patch_buf[768];
            const char *enc_patch = NULL;
            rkvc_err perr = rkvc_qppatch_resolve(d->mlvc_qp_patch_dir, "enc",
                                                 d->mlvc_qp, enc_patch_buf,
                                                 sizeof enc_patch_buf, &enc_patch);
            if (perr != RKVC_OK)
                return perr;
            rkvc_mlvc_enc_config ec = {
                .enc_model_path    = d->mlvc_enc_model_path,
                .gaussian_pmf_path = d->mlvc_gaussian_pmf_path,
                .bitest_pmf_path   = d->mlvc_bitest_pmf_path,
                .qp                = d->mlvc_qp,
                .qp_patch_path     = enc_patch,
            };
            rkvc_err err = rkvc_mlvc_enc_open(&s->mlvc_enc, &ec);
            if (err != RKVC_OK)
                return err;
            int ew = rkvc_mlvc_enc_width(s->mlvc_enc);
            int eh = rkvc_mlvc_enc_height(s->mlvc_enc);
            if (ew > 0 && eh > 0) {
                s->desc.width  = ew;
                s->desc.height = eh;
            }
            rkvc_mlvc_mux_config mc = {
                .output_path = d->output_path,
                .width       = ew,
                .height      = eh,
                .fps_num     = d->fps_num,
                .fps_den     = d->fps_den,
                .qp          = d->mlvc_qp,
            };
            err = rkvc_mlvc_mux_open(&s->mlvc_mux, &mc);
            if (err != RKVC_OK)
                return err;
            goto mlvc_output_done;
        }
#endif

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
        if (err != RKVC_OK) {
            return err;
        }
    }
mlvc_output_done:
    ;

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
    rkvc_err init_err = rkvc_init();
    if (init_err != RKVC_OK)
        return init_err;

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

    /* ── 编解码器独立选择（按方向修正路由）──────────────────────────
     * MLVC 是与 264/265 平行的端到端 AI 编解码器，而非转码中间件：
     *   解码后端 ← 输入（.mlvc 输入 → MLVC 解码；否则标准解码）
     *   编码后端 ← 输出（.mlvc 输出 → MLVC 编码；.yuv 输出 → 无编码器；
     *                     标准容器 → 标准编码器）
     * 三种一等操作：编码 video→.mlvc / 解码 .mlvc→.yuv / 转码 .mlvc→容器 */
#ifdef RKVC_ENABLE_MLVC
    {
        int in_mlvc  = session_is_mlvc_file(s->desc.input_path);
        int out_mlvc = session_is_mlvc_file(s->desc.output_path);
        int out_raw  = session_is_raw_out(s->desc.output_path);
        if (in_mlvc) {
            s->route.dec_backend = RKVC_DEC_BACKEND_MLVC;
            s->route.dec_name    = "mlvc";
        } else if (out_mlvc) {
            /* 编码：标准视频输入由 MPP 解码 */
            s->route.dec_name    = "mpp";
        }
        if (out_mlvc) {
            s->route.enc_backend = RKVC_ENC_BACKEND_MLVC;
            s->route.enc_name    = "mlvc";
            s->route.reason      = "mlvc neural codec encode";
        } else if (in_mlvc && out_raw) {
            s->route.enc_name    = "raw";
            s->route.reason      = "mlvc decode -> raw yuv";
        } else if (in_mlvc) {
            s->route.reason      = "mlvc decode + standard encode (transcode)";
        }
    }
#endif

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

    pthread_mutex_lock(&session->lock);
    session->running = 1;
    session->stats.running = 1;
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

rkvc_err rkvc_session_stop(rkvc_session *session)
{
    if (!session)
        return RKVC_ERR_INVALID;
    session->stop_requested = 1;
    pthread_mutex_lock(&session->lock);
    session->running = 0;
    session->stats.running = 0;
    pthread_mutex_unlock(&session->lock);
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

    /* lock 仅用于同步统计快照，不修改会话逻辑状态；const 弃用符合公共只读契约 */
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

static void session_push_output(rkvc_session *s, rkvc_buffer *buf)
{
    pthread_mutex_lock(&s->lock);
    s->stats.bytes_out += buf ? buf->size : 0;
    pthread_mutex_unlock(&s->lock);

    rkvc_err perr = rkvc_port_push(&s->port_output, buf);
    if (perr == RKVC_ERR_AGAIN)
        rkvc_session_stats_drop(s);
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
        err = session_write_packet(s, pkt);
        session_push_output(s, pkt);
        rkvc_session_stats_tick(s, 1);
        rkvc_buffer_unref(pkt);
        if (err != RKVC_OK)
            return err;
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

        err = session_write_packet(s, pkt);
        session_push_output(s, pkt);
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
        session_push_output(s, display);
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
