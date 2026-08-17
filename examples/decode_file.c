/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** decode_file.c — session 解码示例 */
#include "rkvc/rkvc.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.mp4 out.nv12\n", argv[0]); return 1; }
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path = argv[1]; d.output_path = argv[2];
    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }
    rkvc_err e = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return e == RKVC_OK ? 0 : 1;
}
