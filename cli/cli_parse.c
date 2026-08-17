/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "cli_parse.h"

#include <stdio.h>
#include <string.h>

int rkvc_cli_parse_policy(const char *s, rkvc_policy *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "realtime") == 0) {
        *out = RKVC_POLICY_REALTIME;
        return 0;
    }
    if (strcmp(s, "balanced") == 0) {
        *out = RKVC_POLICY_BALANCED;
        return 0;
    }
    if (strcmp(s, "quality") == 0) {
        *out = RKVC_POLICY_QUALITY;
        return 0;
    }
    if (strcmp(s, "offline") == 0) {
        *out = RKVC_POLICY_OFFLINE;
        return 0;
    }
    if (strcmp(s, "neural") == 0) {
        *out = RKVC_POLICY_NEURAL;
        return 0;
    }
    return -1;
}

int rkvc_cli_parse_wxh(const char *s, int *w, int *h)
{
    int tw = 0, th = 0;
    char extra;
    if (!s || !w || !h)
        return -1;
    if (sscanf(s, "%dx%d%c", &tw, &th, &extra) != 2 || tw <= 0 || th <= 0)
        return -1;
    *w = tw;
    *h = th;
    return 0;
}

int rkvc_cli_parse_rc_mode(const char *s, rkvc_rc_mode *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "vbr") == 0 || strcmp(s, "VBR") == 0) {
        *out = RKVC_RC_VBR;
        return 0;
    }
    if (strcmp(s, "cbr") == 0 || strcmp(s, "CBR") == 0) {
        *out = RKVC_RC_CBR;
        return 0;
    }
    if (strcmp(s, "cqp") == 0 || strcmp(s, "CQP") == 0 ||
        strcmp(s, "fixqp") == 0) {
        *out = RKVC_RC_CQP;
        return 0;
    }
    return -1;
}

int rkvc_cli_parse_codec(const char *s, rkvc_codec *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "auto") == 0) {
        *out = RKVC_CODEC_AUTO;
        return 0;
    }
    if (strcmp(s, "h264") == 0) {
        *out = RKVC_CODEC_H264;
        return 0;
    }
    if (strcmp(s, "hevc") == 0) {
        *out = RKVC_CODEC_HEVC;
        return 0;
    }
    if (strcmp(s, "av1") == 0) {
        *out = RKVC_CODEC_AV1;
        return 0;
    }
    if (strcmp(s, "mlvc") == 0) {
        *out = RKVC_CODEC_MLVC;
        return 0;
    }
    return -1;
}

int rkvc_cli_parse_pix_fmt(const char *s, rkvc_pix_fmt *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "nv12") == 0 || strcmp(s, "NV12") == 0) {
        *out = RKVC_PIX_FMT_NV12;
        return 0;
    }
    if (strcmp(s, "yuv420p") == 0 || strcmp(s, "YUV420P") == 0) {
        *out = RKVC_PIX_FMT_YUV420P;
        return 0;
    }
    return -1;
}
