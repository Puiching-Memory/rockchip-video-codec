// SPDX-License-Identifier: AGPL-3.0-or-later
// rkvc CLI: caps probing and raw-file encode sessions over the C ABI.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "args.hpp"
#include "rkvc/rkvc.h"

namespace {

using cli::usage;

void print_diag(rkvc_diagnostic* d) {
    if (!d)
        return;
    char buf[2048];
    rkvc_diag_fmt_text(d, buf, sizeof(buf));
    fprintf(stderr, "%s", buf);
    rkvc_diag_release(d);
}

int cmd_caps(const cli::Args& a) {
    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    std::vector<const char*> dirs;
    for (const auto& d : a.backend_dirs)
        dirs.push_back(d.c_str());
    opts.backend_dirs = dirs.empty() ? nullptr : dirs.data();
    opts.backend_dir_count = dirs.size();
    rkvc_context* ctx = nullptr;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "caps: context create failed\n");
        return 2;
    }
    rkvc_caps caps;
    memset(&caps, 0, sizeof(caps));
    caps.struct_size = sizeof(caps);
    caps.version = RKVC_ABI_VERSION;
    if (rkvc_probe_device(ctx, &caps) != RKVC_OK) {
        fprintf(stderr, "caps: probe failed\n");
        rkvc_context_destroy(ctx);
        return 2;
    }
    printf("soc=%s mpp_enc=%d mpp_dec=%d rknn=%d npu_cores=%u\n", caps.soc,
           caps.has_mpp_encoder, caps.has_mpp_decoder, caps.has_rknn,
           caps.npu_cores);
    rkvc_context_destroy(ctx);
    return 0;
}

rkvc_codec parse_codec(const std::string& s) {
    if (s == "h264")
        return RKVC_CODEC_H264;
    if (s == "hevc")
        return RKVC_CODEC_HEVC;
    if (s == "av1")
        return RKVC_CODEC_AV1;
    if (s == "mlvc")
        return RKVC_CODEC_MLVC;
    return RKVC_CODEC_AUTO;
}

int cmd_encode(const cli::Args& a) {
    if (a.codec.empty() || a.input.empty() || a.output.empty() || !a.width ||
        !a.height || (a.pixfmt != "nv12" && a.pixfmt != "yuv420p")) {
        usage();
        return 1;
    }
    rkvc_codec codec = parse_codec(a.codec);
    if (codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "encode: unknown codec '%s'\n", a.codec.c_str());
        return 1;
    }
    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts));
    std::vector<const char*> dirs;
    for (const auto& d : a.backend_dirs)
        dirs.push_back(d.c_str());
    opts.backend_dirs = dirs.empty() ? nullptr : dirs.data();
    opts.backend_dir_count = dirs.size();
    rkvc_context* ctx = nullptr;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "encode: context create failed\n");
        return 2;
    }
    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    req.operation = RKVC_OP_ENCODE;
    req.codec = codec;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = a.input.c_str();
    req.input.fmt = (a.pixfmt == "nv12") ? RKVC_FRAME_FMT_NV12
                                         : RKVC_FRAME_FMT_YUV420P;
    req.input.width = a.width;
    req.input.height = a.height;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = a.output.c_str();
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    req.quality.qp = a.qp;
    req.quality.bitrate_bps =
        (a.bitrate > INT32_MAX) ? INT32_MAX : (int32_t)a.bitrate;
    req.quality.gop_size = a.gop;
    rkvc_session* s = nullptr;
    rkvc_diagnostic* diag = nullptr;
    rkvc_status st = rkvc_session_create(ctx, &req, &s, &diag);
    if (st != RKVC_OK) {
        fprintf(stderr, "encode: session create failed: %s\n",
                rkvc_status_str(st));
        print_diag(diag);
        rkvc_context_destroy(ctx);
        return 2;
    }
    st = rkvc_session_start(s, &diag);
    if (st != RKVC_OK) {
        fprintf(stderr, "encode: session start failed: %s\n",
                rkvc_status_str(st));
        print_diag(diag);
        rkvc_session_destroy(s);
        rkvc_context_destroy(ctx);
        return 2;
    }
    st = rkvc_session_wait(s);
    if (st != RKVC_OK)
        fprintf(stderr, "encode: pipeline ended: %s\n", rkvc_status_str(st));
    rkvc_session_destroy(s);
    rkvc_context_destroy(ctx);
    return st == RKVC_OK ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::string cmd;
    cli::Args a;
    if (!cli::parse_args(argc, argv, cmd, a)) {
        cli::usage();
        return 1;
    }
    if (cmd == "caps")
        return cmd_caps(a);
    return cmd_encode(a);
}
