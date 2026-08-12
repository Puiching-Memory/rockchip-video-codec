/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** transcode.c — session 转码示例 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s in.mp4 out.mp4 [policy]\n", argv[0]); return 1; }
    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.input_path = argv[1]; d.output_path = argv[2];
    if (argc > 3) {
        if (strcmp(argv[3], "realtime") == 0) d.policy = RKVC_POLICY_REALTIME;
        else if (strcmp(argv[3], "balanced") == 0) d.policy = RKVC_POLICY_BALANCED;
        else if (strcmp(argv[3], "quality") == 0) d.policy = RKVC_POLICY_QUALITY;
        else if (strcmp(argv[3], "offline") == 0) d.policy = RKVC_POLICY_OFFLINE;
    }
    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_route_plan plan;
    rkvc_session_get_route(s, &plan);
    printf("route: %s -> %s\n", plan.dec_name, plan.enc_name);
    rkvc_err e = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return e == RKVC_OK ? 0 : 1;
}
