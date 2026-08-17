/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_bench.c
 * @brief Session E2E 基准：对比 REALTIME / BALANCED / QUALITY / OFFLINE 路线。
 */

#include "rkvc/rkvc.h"
#include "cli_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/time.h>

typedef struct {
    const char *input;
    const char *output_dir;
    int width;
    int height;
    int frames;
} bench_opts;

static double bench_policy(rkvc_policy policy, const bench_opts *o)
{
    char out[512];
    snprintf(out, sizeof(out), "%s/bench_%s.mp4", o->output_dir,
             rkvc_policy_name(policy));

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.policy      = policy;
    d.input_path  = o->input;
    d.output_path = out;
    d.width       = o->width;
    d.height      = o->height;

    rkvc_session *s = NULL;
    if (rkvc_session_create(&d, &s) != RKVC_OK)
        return -1.0;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    rkvc_err err = rkvc_session_run_file(s);
    gettimeofday(&t1, NULL);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    rkvc_session_destroy(s);

    if (err != RKVC_OK)
        return -1.0;

    double sec = (t1.tv_sec - t0.tv_sec) +
                 (t1.tv_usec - t0.tv_usec) / 1e6;
    return sec > 0 ? st.frames_out / sec : st.avg_fps;
}

int main(int argc, char **argv)
{
    bench_opts o = {
        .input = NULL,
        .output_dir = "/tmp/rkvc_bench",
        .width = 1920,
        .height = 1080,
        .frames = 300,
    };

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"size", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "i:o:s:", opts, NULL)) != -1) {
        if (c == 'i') o.input = optarg;
        else if (c == 'o') o.output_dir = optarg;
        else if (c == 's') {
            if (rkvc_cli_parse_wxh(optarg, &o.width, &o.height) < 0) {
                fprintf(stderr, "invalid size: %s (expected WxH)\n", optarg);
                return 1;
            }
        }
    }

    if (!o.input) {
        fprintf(stderr,
                "usage: rkvc_bench -i INPUT.mp4 [-o OUTDIR] [-s WxH]\n"
                "  generate input: ./example_encode_file -o test.mp4 -s 1920x1080 -n 100\n");
        return 1;
    }

    struct stat st;
    if (stat(o.input, &st) != 0) {
        fprintf(stderr, "input not found: %s\n", o.input);
        return 1;
    }

    rkvc_init();
    mkdir(o.output_dir, 0755);

    static const struct {
        rkvc_policy policy;
        const char *label;
    } routes[] = {
        { RKVC_POLICY_REALTIME, "REALTIME (H.264)" },
        { RKVC_POLICY_BALANCED, "BALANCED (HEVC)" },
        { RKVC_POLICY_QUALITY,  "QUALITY (AV1)" },
        { RKVC_POLICY_OFFLINE,  "OFFLINE (AV1 HQ)" },
        /* NEURAL/MLVC 需要模型与 PMF，由 bench/ RD 套件单独跑，不塞进这条 mp4 转码表。 */
    };

    int failed = 0;
    printf("rkvc session E2E bench (input=%s)\n", o.input);
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        double fps = bench_policy(routes[i].policy, &o);
        printf("  %-18s %.1f fps\n", routes[i].label, fps);
        if (fps < 0.0)
            failed = 1;
    }

    return failed ? 1 : 0;
}
