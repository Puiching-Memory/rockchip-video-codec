/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_session_upscale.c
 * @brief Bench/CLI：Session 硬解 (DMABUF) + RGA 后处理上采样 → NV12 文件。
 */

#include "rkvc/rkvc.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void)
{
    fprintf(stderr,
            "Usage: rkvc_session_upscale -i INPUT -o OUT.nv12 "
            "--width W --height H "
            "[--enc-scale-denom N] [--post-upscale nearest|bilinear|bicubic|rkvc_sr] "
            "--rkvc-sr-model PATH (required for rkvc_sr) [--print-timing]\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *output = NULL;
    const char *upscale_name = "bilinear";
    const char *rkvc_sr_model = NULL;
    int width = 0;
    int height = 0;
    int enc_scale_denom = 1;
    int print_timing = 0;

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"width", required_argument, 0, 'W'},
        {"height", required_argument, 0, 'H'},
        {"enc-scale-denom", required_argument, 0, 'S'},
        {"post-upscale", required_argument, 0, 'U'},
        {"rkvc-sr-model", required_argument, 0, 'M'},
        {"print-timing", no_argument, 0, 'T'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:W:H:S:U:M:Th", opts, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 'W': width = atoi(optarg); break;
        case 'H': height = atoi(optarg); break;
        case 'S': enc_scale_denom = atoi(optarg); break;
        case 'U': upscale_name = optarg; break;
        case 'M': rkvc_sr_model = optarg; break;
        case 'T': print_timing = 1; break;
        default:
            usage();
            return c == 'h' ? 0 : 1;
        }
    }

    if (!input || !output || width <= 0 || height <= 0) {
        usage();
        return 1;
    }

    rkvc_upscale_algo algo = RKVC_UPSCALE_BILINEAR;
    if (rkvc_upscale_algo_from_name(upscale_name, &algo) != 0) {
        fprintf(stderr, "unknown post-upscale algo: %s\n", upscale_name);
        return 1;
    }
    if (algo == RKVC_UPSCALE_AI_SR &&
        (!rkvc_sr_model || !rkvc_sr_model[0])) {
        fprintf(stderr, "--rkvc-sr-model is required when --post-upscale rkvc_sr\n");
        return 1;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path         = input;
    d.output_path        = output;
    d.width              = width;
    d.height             = height;
    d.pixel_format       = RKVC_PIX_FMT_NV12;
    d.enc_scale_denom    = enc_scale_denom > 0 ? enc_scale_denom : 1;
    d.post_upscale_algo  = algo;
    d.post_upscale_rkvc_model_path = rkvc_sr_model;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }

    err = rkvc_session_run_file(s);

    rkvc_session_stats st = {0};
    rkvc_session_get_stats(s, &st);
    rkvc_session_destroy(s);

    if (err != RKVC_OK) {
        fprintf(stderr, "session decode+upscale failed: %s\n", rkvc_err_str(err));
        return 1;
    }

    if (print_timing)
        printf("decode_sec=%.3f rga_sec=%.3f write_sec=%.3f postproc_sec=%.3f\n",
               st.decode_sec, st.rga_sec, st.write_sec, st.postproc_sec);

    return 0;
}
