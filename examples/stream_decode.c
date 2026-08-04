/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** stream_decode.c — v2 解码到 NV12 */
#include "rkvc/rkvc.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc < 3) return 1;
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    d.input_path = argv[1]; d.output_path = argv[2];
    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err e = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return e == RKVC_OK ? 0 : 1;
}
