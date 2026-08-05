/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file pipeline.c
 * @brief 管线模板与默认参数。
 */

#include "internal.h"

rkvc_pipeline_desc rkvc_pipeline_desc_defaults(void)
{
    rkvc_pipeline_desc d;
    memset(&d, 0, sizeof(d));
    d.template_id   = RKVC_TEMPLATE_FILE_TRANSCODE;
    d.policy        = RKVC_POLICY_BALANCED;
    d.codec         = RKVC_CODEC_AUTO;
    d.width         = 1920;
    d.height        = 1080;
    d.fps_num       = 30;
    d.fps_den       = 1;
    d.bitrate       = 4000000;
    d.pixel_format  = RKVC_PIX_FMT_NV12;
    d.gop_size      = 60;
    d.low_latency   = 0;
    d.queue_depth   = RKVC_PORT_QUEUE_DEFAULT;
    d.rc_mode       = RKVC_RC_CBR;
    d.qp_init       = -1;
    d.enc_scale_denom     = 1;
    d.post_upscale_algo   = RKVC_UPSCALE_NONE;
    d.post_upscale_rkvc_model_path = NULL;
    d.svt_lp              = RKVC_SVT_LP_AUTO;
    d.svt_rtc             = 0;
    d.capture_device      = NULL;
    d.capture_max_frames  = 0;
    d.capture_timeout_ms  = 1000;
    return d;
}

rkvc_err rkvc_pipeline_from_template(rkvc_pipeline_template tmpl,
                                     rkvc_pipeline_desc *desc)
{
    if (!desc)
        return RKVC_ERR_INVALID;

    *desc = rkvc_pipeline_desc_defaults();
    desc->template_id = tmpl;

    switch (tmpl) {
    case RKVC_TEMPLATE_FILE_ENCODE:
        desc->policy = RKVC_POLICY_REALTIME;
        break;
    case RKVC_TEMPLATE_FILE_DECODE:
        desc->policy = RKVC_POLICY_BALANCED;
        break;
    case RKVC_TEMPLATE_FILE_TRANSCODE:
        desc->policy = RKVC_POLICY_BALANCED;
        break;
    case RKVC_TEMPLATE_LIVE_CAPTURE:
        desc->policy = RKVC_POLICY_REALTIME;
        desc->low_latency = 1;
        break;
    case RKVC_TEMPLATE_AV1_STORAGE:
        desc->policy = RKVC_POLICY_QUALITY;
        desc->codec  = RKVC_CODEC_AV1;
        break;
    default:
        return RKVC_ERR_INVALID;
    }

    return RKVC_OK;
}
