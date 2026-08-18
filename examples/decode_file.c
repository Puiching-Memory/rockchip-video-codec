/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file decode_file.c
 * @brief 最简文件解码：容器 → 原始 YUV，可选输出像素格式。
 *
 * 硬件解不出请求格式时（如 8-bit HEVC 硬解只出 NV12），库内经 swscale 转换。
 *
 * 用法: example_decode_file in.mp4 out.nv12 [nv12|yuv420p|nv16|p010]
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <string.h>

static const struct {
    const char   *name;
    rkvc_pix_fmt  fmt;
} k_pix_fmts[] = {
    { "nv12",    RKVC_PIX_FMT_NV12 },
    { "yuv420p", RKVC_PIX_FMT_YUV420P },
    { "nv16",    RKVC_PIX_FMT_NV16 },
    { "p010",    RKVC_PIX_FMT_P010 },
};

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.mp4 out.nv12 [nv12|yuv420p|nv16|p010]\n", argv[0]);
        return 1;
    }

    rkvc_pix_fmt fmt = RKVC_PIX_FMT_NV12;
    if (argc > 3) {
        int found = 0;
        for (size_t i = 0; i < sizeof(k_pix_fmts) / sizeof(k_pix_fmts[0]); i++) {
            if (strcmp(argv[3], k_pix_fmts[i].name) == 0) {
                fmt = k_pix_fmts[i].fmt;
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "invalid pix-fmt: %s\n", argv[3]);
            return 1;
        }
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path   = argv[1];
    d.output_path  = argv[2];
    d.pixel_format = fmt;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }

    err = rkvc_session_run_file(s);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("decode %s -> %s frames=%llu avg_fps=%.2f err=%s\n",
           argv[1], argv[2], (unsigned long long)st.frames_out,
           st.avg_fps, rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
