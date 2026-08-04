/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** stream_transcode.c — v2 文件转码（同 transcode） */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *in = argc > 1 ? argv[1] : "input.mp4";
    const char *out = argc > 2 ? argv[2] : "output.mp4";
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.input_path = in; d.output_path = out;
    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err e = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return e == RKVC_OK ? 0 : 1;
}
