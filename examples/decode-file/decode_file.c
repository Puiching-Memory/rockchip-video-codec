// SPDX-License-Identifier: AGPL-3.0-or-later
// 基于 rkvc C ABI 的 FILE 端点解码（等价于 `rkvc decode`）。
//
// 用法：
//   decode_file <backend_dir> <h264|hevc|av1|mlvc> <width> <height>
//               <nv12|yuv420p> <in.bit> <out.raw> [model_dir ...]
//
// width/height 描述编码图像；裸输出保持相同几何。MLVC 解码需要注册其
// 解码器模型（--model-id 是 bundle 内文件词干，如 MLVCDecoder_rk3576）；
// 模型目录按位置参数传入。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkvc/rkvc.h"

static int fail(const char *what, rkvc_status st, rkvc_diagnostic *diag) {
    fprintf(stderr, "decode_file: %s failed: %s\n", what, rkvc_status_str(st));
    if (diag) {
        char buf[2048];
        rkvc_diag_fmt_text(diag, buf, sizeof(buf));
        fprintf(stderr, "%s", buf);
        rkvc_diag_release(diag);
    }
    return 2;
}

static rkvc_codec parse_codec(const char *s) {
    if (!strcmp(s, "h264"))
        return RKVC_CODEC_H264;
    if (!strcmp(s, "hevc"))
        return RKVC_CODEC_HEVC;
    if (!strcmp(s, "av1"))
        return RKVC_CODEC_AV1;
    if (!strcmp(s, "mlvc"))
        return RKVC_CODEC_MLVC;
    return RKVC_CODEC_AUTO;
}

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr,
                "usage: %s <backend_dir> <codec> <width> <height> <pixfmt> "
                "<in.bit> <out.raw> [model_dir ...]\n",
                argv[0]);
        return 1;
    }
    rkvc_codec codec = parse_codec(argv[2]);
    uint32_t width = (uint32_t)strtoul(argv[3], NULL, 10);
    uint32_t height = (uint32_t)strtoul(argv[4], NULL, 10);
    rkvc_frame_fmt raw = RKVC_FRAME_FMT_UNKNOWN;
    if (!strcmp(argv[5], "nv12"))
        raw = RKVC_FRAME_FMT_NV12;
    else if (!strcmp(argv[5], "yuv420p"))
        raw = RKVC_FRAME_FMT_YUV420P;
    if (codec == RKVC_CODEC_AUTO || !width || !height ||
        raw == RKVC_FRAME_FMT_UNKNOWN) {
        fprintf(stderr, "decode_file: bad codec, geometry, or pixfmt\n");
        return 1;
    }

    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    const char *dirs[1] = {argv[1]};
    opts.backend_dirs = dirs;
    opts.backend_dir_count = 1;
    rkvc_context *ctx = NULL;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "decode_file: context create failed\n");
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
    req.operation = RKVC_OP_DECODE;
    req.codec = codec;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = argv[6];
    req.input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    req.input.width = width;
    req.input.height = height;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = argv[7];
    req.output.fmt = raw;
    req.output.width = width;
    req.output.height = height;

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
        fprintf(stderr, "decode_file: pipeline ended: %s\n%s",
                rkvc_status_str(st), err);
    }
    rkvc_session_destroy(s);
    rkvc_context_destroy(ctx);
    return st == RKVC_OK ? 0 : 2;
}
