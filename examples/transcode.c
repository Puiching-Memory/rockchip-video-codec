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
        else if (strcmp(argv[3], "neural") == 0) d.policy = RKVC_POLICY_NEURAL;
        else {
            fprintf(stderr, "invalid policy: %s\n", argv[3]);
            return 1;
        }
    }
    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }
    rkvc_route_plan plan;
    rkvc_session_get_route(s, &plan);
    printf("route: %s -> %s\n", plan.dec_name, plan.enc_name);
    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
