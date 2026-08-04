/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** stream_encode.c — v2 原始文件编码 */
#include "rkvc/rkvc.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s raw.nv12 out.mp4 WxH\n", argv[0]); return 1; }
    int w, h; sscanf(argv[3], "%dx%d", &w, &h);
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path = argv[1]; d.output_path = argv[2];
    d.width = w; d.height = h;
    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err e = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return e == RKVC_OK ? 0 : 1;
}
