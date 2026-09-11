// SPDX-License-Identifier: AGPL-3.0-or-later
// 基于 rkvc C ABI 的 FILE 端点 3 倍上采样（等价于 `rkvc upscale`）。
//
// 用法：
//   upscale_file <backend_dir> <width> <height> <nv12|yuv420p> <model-id>
//                <in.raw> <out.raw> [model_dir ...]
//
// SR 节点固定 3 倍：输出几何为 (3w, 3h)。model id 选择已注册的
// 模型（目录内 .rknn 文件词干，如 phase_rlfn_sr_x3）。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkvc/rkvc.h"

static int fail(const char *what, rkvc_status st, rkvc_diagnostic *diag) {
    fprintf(stderr, "upscale_file: %s failed: %s\n", what, rkvc_status_str(st));
    if (diag) {
        char buf[2048];
        rkvc_diag_fmt_text(diag, buf, sizeof(buf));
        fprintf(stderr, "%s", buf);
        rkvc_diag_release(diag);
    }
    return 2;
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <backend_dir> <width> <height> <pixfmt> <model-id> "
                "<in.raw> <out.raw> [model_dir ...]\n",
                argv[0]);
        return 1;
    }
    uint32_t width = (uint32_t)strtoul(argv[2], NULL, 10);
    uint32_t height = (uint32_t)strtoul(argv[3], NULL, 10);
    rkvc_frame_fmt raw = RKVC_FRAME_FMT_UNKNOWN;
    if (!strcmp(argv[4], "nv12"))
        raw = RKVC_FRAME_FMT_NV12;
    else if (!strcmp(argv[4], "yuv420p"))
        raw = RKVC_FRAME_FMT_YUV420P;
    if (!width || !height || raw == RKVC_FRAME_FMT_UNKNOWN) {
        fprintf(stderr, "upscale_file: bad geometry or pixfmt\n");
        return 1;
    }

    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    const char *dirs[1] = {argv[1]};
    opts.backend_dirs = dirs;
    opts.backend_dir_count = 1;
    rkvc_context *ctx = NULL;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "upscale_file: context create failed\n");
        return 2;
    }
    for (int i = 8; i < argc; i++) {
        rkvc_diagnostic *diag = NULL;
        rkvc_status st = rkvc_context_add_model_dir(ctx, argv[i], &diag);
        if (st != RKVC_OK) {
            int rc = fail("add model", st, diag);
            rkvc_context_destroy(ctx);
            return rc;
        }
    }

    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    req.operation = RKVC_OP_UPSCALE;
    req.codec = RKVC_CODEC_AUTO;
    req.model_id = argv[5];
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = argv[6];
    req.input.fmt = raw;
    req.input.width = width;
    req.input.height = height;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = argv[7];
    req.output.fmt = raw;
    req.output.width = width * 3;
    req.output.height = height * 3;

    rkvc_session *s = NULL;
    rkvc_diagnostic *diag = NULL;
    rkvc_status st = rkvc_session_create(ctx, &req, &s, &diag);
    if (st != RKVC_OK) {
        int rc = fail("session create", st, diag);
        rkvc_context_destroy(ctx);
        return rc;
    }
    st = rkvc_session_start(s, &diag);
    if (st != RKVC_OK) {
        int rc = fail("session start", st, diag);
        rkvc_session_destroy(s);
        rkvc_context_destroy(ctx);
        return rc;
    }
    st = rkvc_session_wait(s);
    if (st != RKVC_OK) {
        char err[2048];
        rkvc_session_error_text(s, err, sizeof(err));
        fprintf(stderr, "upscale_file: pipeline ended: %s\n%s",
                rkvc_status_str(st), err);
    }
    rkvc_session_destroy(s);
    rkvc_context_destroy(ctx);
    return st == RKVC_OK ? 0 : 2;
}
