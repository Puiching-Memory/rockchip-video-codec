/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_transcode.c
 * @brief v2 Session 转码 CLI（供 bench/ RD 基准与脚本调用）。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static rkvc_policy parse_policy(const char *s)
{
    if (!s || strcmp(s, "realtime") == 0) return RKVC_POLICY_REALTIME;
    if (strcmp(s, "balanced") == 0) return RKVC_POLICY_BALANCED;
    if (strcmp(s, "quality") == 0) return RKVC_POLICY_QUALITY;
    if (strcmp(s, "offline") == 0) return RKVC_POLICY_OFFLINE;
    return RKVC_POLICY_BALANCED;
}

static int parse_rc_mode(const char *s, rkvc_rc_mode *out)
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

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *policy_s = "balanced";
    const char *rc_mode_s = NULL;
    int64_t bitrate = 4000000;
    int w = 0, h = 0, qp = -1;
    int svt_lp = 0;
    int svt_rtc = 0;

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"policy", required_argument, 0, 'p'},
        {"bitrate", required_argument, 0, 'b'},
        {"size", required_argument, 0, 's'},
        {"rc-mode", required_argument, 0, 'R'},
        {"qp", required_argument, 0, 'q'},
        {"svt-lp", required_argument, 0, 'L'},
        {"svt-rtc", no_argument, 0, 'C'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:p:b:s:R:q:L:C", opts, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 'p': policy_s = optarg; break;
        case 'b': bitrate = atoll(optarg); break;
        case 's': sscanf(optarg, "%dx%d", &w, &h); break;
        case 'R':
            rc_mode_s = optarg;
            break;
        case 'q':
            qp = atoi(optarg);
            break;
        case 'L':
            svt_lp = atoi(optarg);
            break;
        case 'C':
            svt_rtc = 1;
            break;
        default:
            fprintf(stderr,
                    "usage: rkvc_transcode -i IN -o OUT [-p realtime|balanced|quality|offline] "
                    "[-b bps] [-s WxH] [--rc-mode vbr|cbr|cqp] [--qp N] "
                    "[--svt-lp N] [--svt-rtc]\n");
            return 1;
        }
    }

    if (!input || !output) {
        fprintf(stderr,
                "usage: rkvc_transcode -i IN -o OUT [-p realtime|balanced|quality|offline] "
                "[-b bps] [-s WxH] [--rc-mode vbr|cbr|cqp] [--qp N] "
                "[--svt-lp N] [--svt-rtc]\n");
        return 1;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.policy      = parse_policy(policy_s);
    d.input_path  = input;
    d.output_path = output;
    d.bitrate     = bitrate;
    d.svt_lp      = svt_lp;
    d.svt_rtc     = svt_rtc;
    if (w > 0 && h > 0) {
        d.width  = w;
        d.height = h;
    }
    if (rc_mode_s) {
        if (parse_rc_mode(rc_mode_s, &d.rc_mode) < 0) {
            fprintf(stderr, "invalid rc-mode: %s\n", rc_mode_s);
            return 1;
        }
    }
    if (qp >= 0)
        d.qp_init = qp;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK)
        return 1;

    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
