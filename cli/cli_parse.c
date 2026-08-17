/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "cli_parse.h"

#include <stdio.h>
#include <string.h>

int rkvc_cli_parse_policy(const char *s, rkvc_policy *out)
{
    if (!s || !out)
        return -1;
    for (int i = 0; i <= RKVC_POLICY_NEURAL; i++) {
        if (strcmp(s, rkvc_policy_name((rkvc_policy)i)) == 0) {
            *out = (rkvc_policy)i;
            return 0;
        }
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
    for (int i = 0; i <= RKVC_CODEC_AUTO; i++) {
        if (strcmp(s, rkvc_codec_name((rkvc_codec)i)) == 0) {
            *out = (rkvc_codec)i;
            return 0;
        }
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
