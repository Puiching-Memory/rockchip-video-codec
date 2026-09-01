/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "rkvc/rkvc.h"

#include <stdio.h>

int main(void) {
    rkvc_context *ctx = NULL;
    rkvc_job *job = NULL;
    rkvc_diag *diag = NULL;
    rkvc_request req;
    rkvc_status st = rkvc_context_create(NULL, &ctx);

    if (st == RKVC_STATUS_OK) {
        rkvc_request_init(&req, sizeof(req));
        req.operation = RKVC_OPERATION_UPSCALE;
        req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
        req.input.fmt = RKVC_FRAME_FMT_NV12;
        req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
        req.output.fmt = RKVC_FRAME_FMT_NV12;
        req.width = 1280;
        req.height = 960;
        st = rkvc_job_create(ctx, &req, &diag, &job);
    }
    if (st == RKVC_STATUS_OK)
        puts("upscale transform selected; feed frames with rkvc_job_push()");
    else
        fprintf(stderr, "no installed upscale backend: %s\n",
                rkvc_status_str(st));
    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    return st == RKVC_STATUS_OK ? 0 : 1;
}
