/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file router.c
 * @brief Codec Router：H.264 / HEVC / AV1 三族 policy 路由。
 */

#include "internal.h"

const char *rkvc_codec_name(rkvc_codec codec)
{
    switch (codec) {
    case RKVC_CODEC_H264: return "h264";
    case RKVC_CODEC_HEVC: return "hevc";
    case RKVC_CODEC_AV1:  return "av1";
    case RKVC_CODEC_MLVC: return "mlvc";
    default:              return "auto";
    }
}

const char *rkvc_policy_name(rkvc_policy policy)
{
    switch (policy) {
    case RKVC_POLICY_REALTIME: return "realtime";
    case RKVC_POLICY_BALANCED: return "balanced";
    case RKVC_POLICY_QUALITY:  return "quality";
    case RKVC_POLICY_OFFLINE:  return "offline";
    case RKVC_POLICY_NEURAL:   return "neural";
    default:                   return "unknown";
    }
}

static void fill_mpp_h264(rkvc_route_plan *plan, const char *reason)
{
    plan->codec        = RKVC_CODEC_H264;
    plan->enc_backend  = RKVC_ENC_BACKEND_MPP;
    plan->dec_backend  = RKVC_DEC_BACKEND_MPP;
    plan->enc_name     = "h264_rkmpp";
    plan->dec_name     = "h264_rkmpp";
    plan->svt_preset   = 0;
    plan->reason       = reason;
}

static void fill_mpp_hevc(rkvc_route_plan *plan, const char *reason)
{
    plan->codec        = RKVC_CODEC_HEVC;
    plan->enc_backend  = RKVC_ENC_BACKEND_MPP;
    plan->dec_backend  = RKVC_DEC_BACKEND_MPP;
    plan->enc_name     = "hevc_rkmpp";
    plan->dec_name     = "hevc_rkmpp";
    plan->svt_preset   = 0;
    plan->reason       = reason;
}

static void fill_av1_svt(rkvc_route_plan *plan, int preset, const char *reason)
{
    plan->codec        = RKVC_CODEC_AV1;
    plan->enc_backend  = RKVC_ENC_BACKEND_SVT;
    plan->dec_backend  = RKVC_DEC_BACKEND_MPP;
    plan->enc_name     = "libsvtav1";
    plan->dec_name     = "av1_rkmpp";
    plan->svt_preset   = preset;
    plan->reason       = reason;
}

static void fill_mlvc(rkvc_route_plan *plan, const char *reason)
{
    plan->codec        = RKVC_CODEC_MLVC;
    plan->enc_backend  = RKVC_ENC_BACKEND_MLVC;
    plan->dec_backend  = RKVC_DEC_BACKEND_MLVC;
    plan->enc_name     = "mlvc";
    plan->dec_name     = "mlvc";
    plan->svt_preset   = 0;
    plan->reason       = reason;
}

rkvc_err rkvc_route_resolve(const rkvc_pipeline_desc *desc,
                            rkvc_route_plan *plan)
{
    if (!desc || !plan)
        return RKVC_ERR_INVALID;

    memset(plan, 0, sizeof(*plan));

    if (desc->codec != RKVC_CODEC_AUTO) {
        switch (desc->codec) {
        case RKVC_CODEC_H264:
            fill_mpp_h264(plan, "forced h264");
            return RKVC_OK;
        case RKVC_CODEC_HEVC:
            fill_mpp_hevc(plan, "forced hevc");
            return RKVC_OK;
        case RKVC_CODEC_AV1:
            fill_av1_svt(plan, RKVC_SVT_PRESET_PERF, "forced av1");
            return RKVC_OK;
        case RKVC_CODEC_MLVC:
            fill_mlvc(plan, "forced mlvc neural codec");
            return RKVC_OK;
        default:
            return RKVC_ERR_INVALID;
        }
    }

    const int pixels = desc->width * desc->height;
    const int high_res = (pixels >= 1920 * 1080);

    switch (desc->policy) {
    case RKVC_POLICY_REALTIME:
        fill_mpp_h264(plan, "realtime: h264_rkmpp E2E ~36fps@1080p");
        break;

    case RKVC_POLICY_BALANCED:
        if (high_res && desc->fps_num >= 50)
            fill_mpp_h264(plan, "balanced high-fps 1080p+: h264");
        else
            fill_mpp_hevc(plan, "balanced: hevc_rkmpp E2E ~27fps@1080p");
        break;

    case RKVC_POLICY_QUALITY:
        fill_av1_svt(plan, RKVC_SVT_PRESET_PERF,
                      "quality: SVT preset 11 + av1_rkmpp");
        break;

    case RKVC_POLICY_OFFLINE:
        fill_av1_svt(plan, RKVC_SVT_PRESET_HQ,
                      "offline: SVT preset 4 + av1_rkmpp (≥1fps@1080p)");
        break;

    case RKVC_POLICY_NEURAL:
        fill_mlvc(plan, "neural: MLVC neural video codec (NPU + rANS)");
        break;

    default:
        return RKVC_ERR_INVALID;
    }

    return RKVC_OK;
}
