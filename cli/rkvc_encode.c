/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_encode.c
 * @brief Session CLI：原始 NV12 → 编码文件。
 */

#include "rkvc/rkvc.h"
#include "cli_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static void usage(void)
{
    printf("rkvc_encode -i raw.nv12 -o out.mp4 -s 1920x1080 [-p realtime|balanced|quality|offline] "
           "[--rc-mode vbr|cbr|cqp] [--qp N] [--enc-scale-denom N] "
           "[--svt-lp N] [--svt-rtc]\n"
           "  Note: --enc-scale-denom downscales before encode only.\n"
           "  Post-upscale (RGA / rkvc_sr) uses rkvc_session_upscale, not this tool.\n"
           "  Policy 'neural' (MLVC) needs --mlvc-* model args; use rkvc_transcode instead.\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *policy_s = "realtime";
    const char *rc_mode_s = NULL;
    const char *pix_fmt_s = NULL;
    int w = 1920, h = 1080, fps = 30, qp = -1;
    int enc_scale_denom = 1;
    int svt_lp = 0;
    int svt_rtc = 0;
    int64_t bitrate = 4000000;

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"size", required_argument, 0, 's'},
        {"rate", required_argument, 0, 'r'},
        {"bitrate", required_argument, 0, 'b'},
        {"policy", required_argument, 0, 'p'},
        {"rc-mode", required_argument, 0, 'R'},
        {"qp", required_argument, 0, 'q'},
        {"pix-fmt", required_argument, 0, 'f'},
        {"enc-scale-denom", required_argument, 0, 'S'},
        {"svt-lp", required_argument, 0, 'L'},
        {"svt-rtc", no_argument, 0, 'C'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:s:r:b:p:hR:q:f:S:L:C", opts, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 's':
            if (rkvc_cli_parse_wxh(optarg, &w, &h) < 0) {
                fprintf(stderr, "invalid size: %s (expected WxH)\n", optarg);
                return 1;
            }
            break;
        case 'r': fps = atoi(optarg); break;
        case 'b': bitrate = atoll(optarg); break;
        case 'p': policy_s = optarg; break;
        case 'R': rc_mode_s = optarg; break;
        case 'q': qp = atoi(optarg); break;
        case 'f': pix_fmt_s = optarg; break;
        case 'S': enc_scale_denom = atoi(optarg); break;
        case 'L': svt_lp = atoi(optarg); break;
        case 'C': svt_rtc = 1; break;
        default: usage(); return c == 'h' ? 0 : 1;
        }
    }

    if (!input || !output) {
        usage();
        return 1;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    if (rkvc_cli_parse_policy(policy_s, &d.policy) < 0) {
        fprintf(stderr, "invalid policy: %s\n", policy_s);
        return 1;
    }
    d.input_path  = input;
    d.output_path = output;
    d.width       = w;
    d.height      = h;
    d.fps_num     = fps;
    d.bitrate     = bitrate;
    if (pix_fmt_s) {
        if (rkvc_cli_parse_pix_fmt(pix_fmt_s, &d.pixel_format) < 0) {
            fprintf(stderr, "invalid pix-fmt: %s\n", pix_fmt_s);
            return 1;
        }
    }
    if (rc_mode_s) {
        if (rkvc_cli_parse_rc_mode(rc_mode_s, &d.rc_mode) < 0) {
            fprintf(stderr, "invalid rc-mode: %s\n", rc_mode_s);
            return 1;
        }
    }
    if (qp >= 0)
        d.qp_init = qp;
    if (enc_scale_denom > 0)
        d.enc_scale_denom = enc_scale_denom;
    d.svt_lp  = svt_lp;
    d.svt_rtc = svt_rtc;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }

    rkvc_route_plan plan;
    rkvc_session_get_route(s, &plan);
    fprintf(stderr, "route: %s -> %s (%s)\n",
            plan.dec_name, plan.enc_name, plan.reason);

    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    if (err != RKVC_OK) {
        fprintf(stderr, "encode failed: %s\n", rkvc_err_str(err));
        return 1;
    }
    return 0;
}
