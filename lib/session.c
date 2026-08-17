/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session.c
 * @brief rkvc 会话生命周期与图构建（循环见 session_run.c）。
 */

#include "internal.h"
#ifdef RKVC_ENABLE_MLVC
#include "qppatch.h"
#endif

#include "session_priv.h"

static rkvc_err port_init(rkvc_port *p, const char *name, int depth,
                          rkvc_session *s)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name);
    p->queue   = rkvc_port_queue_create(depth);
    p->session = s;
    if (!p->queue)
        return RKVC_ERR_NOMEM;
    return RKVC_OK;
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
            rkvc_err szerr = session_enc_size(s, &ew, &eh);
            if (szerr != RKVC_OK)
                return szerr;
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
            rkvc_err szerr = session_enc_size(s, &ew, &eh);
            if (szerr != RKVC_OK)
                return szerr;
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
        rkvc_err szerr = session_enc_size(s, &ew, &eh);
        if (szerr != RKVC_OK)
            return szerr;

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

    if (s->desc.enc_scale_denom > 1) {
        int ew = 0, eh = 0;
        if (session_enc_size(s, &ew, &eh) != RKVC_OK) {
            rkvc_session_destroy(s);
            return RKVC_ERR_INVALID;
        }
    }

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

    if (port_init(&s->port_capture, "capture", s->desc.queue_depth, s) != RKVC_OK ||
        port_init(&s->port_output, "output", s->desc.queue_depth, s) != RKVC_OK ||
        port_init(&s->port_preview, "preview", s->desc.queue_depth, s) != RKVC_OK) {
        rkvc_session_destroy(s);
        return RKVC_ERR_NOMEM;
    }

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
