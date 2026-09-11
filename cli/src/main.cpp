// SPDX-License-Identifier: AGPL-3.0-or-later
// rkvc CLI：通过 C ABI 进行能力探测与裸文件编码会话。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <algorithm>
#endif

#include "args.hpp"
#include "rkvc/model.hpp"
#include "rkvc/rkvc.h"

#ifdef __linux__
#include <dlfcn.h>
#include "rkvc/plugin.hpp"
#endif

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

int cmd_version(const cli::Args& a) {
    if (a.json) {
        printf("{\"version\": \"%d.%d.%d\", \"abi\": %u}\n",
               RKVC_ABI_VERSION_MAJOR, RKVC_ABI_VERSION_MINOR,
               RKVC_ABI_VERSION_PATCH, RKVC_ABI_VERSION);
    } else {
        printf("rkvc %d.%d.%d (abi %u)\n", RKVC_ABI_VERSION_MAJOR,
               RKVC_ABI_VERSION_MINOR, RKVC_ABI_VERSION_PATCH,
               RKVC_ABI_VERSION);
    }
    return 0;
}

std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if ((unsigned char)c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    return o;
}

#ifdef __linux__
std::vector<std::string> scan_dir(const std::string& dir, const char* suffix) {
    std::vector<std::string> out;
    DIR* dp = opendir(dir.c_str());
    if (!dp)
        return out;
    while (dirent* e = readdir(dp)) {
        std::string name = e->d_name;
        size_t n = strlen(suffix);
        if (name.size() < n || name.compare(name.size() - n, n, suffix) != 0)
            continue;
        out.push_back(dir + "/" + name);
    }
    closedir(dp);
    std::sort(out.begin(), out.end());
    return out;
}

int cmd_inspect(const cli::Args& a) {
    if (a.sub == "backends") {
        bool first = true;
        std::string text, json = "{\"backends\": [";
        for (const auto& dir : a.backend_dirs) {
            for (const auto& path : scan_dir(dir, ".so")) {
                void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
                if (!h)
                    continue;
                auto query = (rkvc::PluginQueryFn)dlsym(h, "rkvc_plugin_query");
                const rkvc::PluginDescriptor* d =
                    query ? query(rkvc::kPluginAbi) : nullptr;
                if (!d) {
                    dlclose(h);
                    continue;
                }
                std::string names;
                for (size_t i = 0; i < d->factory_count; ++i) {
                    if (i)
                        names += ",";
                    names += d->factories[i]->id();
                }
                text += path + " " + (d->name ? d->name : "?") + " " +
                        (d->version ? d->version : "?") + " [" + names + "]\n";
                if (!first)
                    json += ", ";
                first = false;
                json += "{\"path\": \"" + json_escape(path) +
                        "\", \"name\": \"" +
                        json_escape(d->name ? d->name : "") +
                        "\", \"version\": \"" +
                        json_escape(d->version ? d->version : "") +
                        "\", \"factories\": \"" + json_escape(names) + "\"}";
                dlclose(h);
            }
        }
        json += "]}";
        printf("%s", a.json ? (json + "\n").c_str() : text.c_str());
        return 0;
    }
    if (a.sub == "models") {
        std::vector<std::string> dirs = a.model_dirs;
        for (const auto& m : a.models)
            dirs.push_back(m);  // --model 直接给目录
        bool first = true;
        std::string text, json = "{\"models\": [";
        for (const auto& dir : dirs) {
            rkvc::Diag d;
            auto ms = rkvc::load_model_dir(dir, &d);
            if (!ms)
                continue;
            for (const rkvc::Model& info : ms.value()) {
                text += dir + " " + info.meta.id + " " + info.meta.family +
                        " " + info.meta.role + " " + info.meta.target + "\n";
                if (!first)
                    json += ", ";
                first = false;
                json += "{\"id\": \"" + json_escape(info.meta.id) +
                        "\", \"role\": \"" + json_escape(info.meta.role) +
                        "\", \"family\": \"" + json_escape(info.meta.family) +
                        "\", \"target\": \"" + json_escape(info.meta.target) +
                        "\", \"path\": \"" + json_escape(dir) + "\"}";
            }
        }
        json += "]}";
        printf("%s", a.json ? (json + "\n").c_str() : text.c_str());
        return 0;
    }
    cli::usage();
    return 1;
}
#else
int cmd_inspect(const cli::Args& a) {
    (void)a;
    fprintf(stderr, "inspect: supported on Linux only\n");
    return 2;
}
#endif

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

int add_model_dir(rkvc_context* ctx, const std::string& dir) {
    rkvc_diagnostic* diag = nullptr;
    rkvc_status st = rkvc_context_add_model_dir(ctx, dir.c_str(), &diag);
    if (st != RKVC_OK) {
        fprintf(stderr, "model: load dir '%s' failed: %s\n", dir.c_str(),
                rkvc_status_str(st));
        print_diag(diag);
        return 2;
    }
    return 0;
}

int load_models(rkvc_context* ctx, const cli::Args& a) {
    for (const auto& m : a.models)
        if (add_model_dir(ctx, m) != 0)
            return 2;
    for (const auto& dir : a.model_dirs)
        if (add_model_dir(ctx, dir) != 0)
            return 2;
    return 0;
}

int run_file_session(const cli::Args& a, rkvc_operation op, const char* tag,
                     rkvc_frame_fmt in_fmt, uint32_t in_w, uint32_t in_h,
                     rkvc_frame_fmt out_fmt, uint32_t out_w, uint32_t out_h,
                     rkvc_codec codec);

int run_file_session(const cli::Args& a, rkvc_operation op, const char* tag,
                     rkvc_frame_fmt in_fmt, uint32_t in_w, uint32_t in_h,
                     rkvc_frame_fmt out_fmt, uint32_t out_w, uint32_t out_h);

int cmd_encode(const cli::Args& a) {
    if (a.codec.empty()) {
        usage();
        return 1;
    }
    rkvc_codec codec = parse_codec(a.codec);
    if (codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "encode: unknown codec '%s'\n", a.codec.c_str());
        return 1;
    }
    rkvc_frame_fmt raw =
        (a.pixfmt == "nv12") ? RKVC_FRAME_FMT_NV12 : RKVC_FRAME_FMT_YUV420P;
    return run_file_session(a, RKVC_OP_ENCODE, "encode", raw, a.width, a.height,
                            RKVC_FRAME_FMT_BITSTREAM, 0, 0, codec);
}

int cmd_decode(const cli::Args& a) {
    if (a.codec.empty()) {
        usage();
        return 1;
    }
    rkvc_codec codec = parse_codec(a.codec);
    if (codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "decode: unknown codec '%s'\n", a.codec.c_str());
        return 1;
    }
    rkvc_frame_fmt raw =
        (a.pixfmt == "nv12") ? RKVC_FRAME_FMT_NV12 : RKVC_FRAME_FMT_YUV420P;
    return run_file_session(a, RKVC_OP_DECODE, "decode",
                            RKVC_FRAME_FMT_BITSTREAM, a.width, a.height, raw,
                            a.width, a.height, codec);
}

int cmd_upscale(const cli::Args& a) {
    rkvc_frame_fmt raw =
        (a.pixfmt == "nv12") ? RKVC_FRAME_FMT_NV12 : RKVC_FRAME_FMT_YUV420P;
    return run_file_session(a, RKVC_OP_UPSCALE, "upscale", raw, a.width,
                            a.height, raw, a.width * 3, a.height * 3,
                            RKVC_CODEC_AUTO);
}

int run_file_session(const cli::Args& a, rkvc_operation op, const char* tag,
                     rkvc_frame_fmt in_fmt, uint32_t in_w, uint32_t in_h,
                     rkvc_frame_fmt out_fmt, uint32_t out_w, uint32_t out_h,
                     rkvc_codec codec) {
    if (a.input.empty() || a.output.empty() || !a.width || !a.height ||
        (a.pixfmt != "nv12" && a.pixfmt != "yuv420p")) {
        usage();
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
        fprintf(stderr, "%s: context create failed\n", tag);
        return 2;
    }
    if (load_models(ctx, a) != 0) {
        rkvc_context_destroy(ctx);
        return 2;
    }
    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    req.operation = op;
    req.codec = codec;
    if (!a.model_id.empty())
        req.model_id = a.model_id.c_str();
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = a.input.c_str();
    req.input.fmt = in_fmt;
    req.input.width = in_w;
    req.input.height = in_h;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = a.output.c_str();
    req.output.fmt = out_fmt;
    req.output.width = out_w;
    req.output.height = out_h;
    req.quality.qp = a.qp;
    req.quality.bitrate_bps =
        (a.bitrate > INT32_MAX) ? INT32_MAX : (int32_t)a.bitrate;
    req.quality.gop_size = a.gop;
    req.quality.fps = a.fps;
    rkvc_session* s = nullptr;
    rkvc_diagnostic* diag = nullptr;
    rkvc_status st = rkvc_session_create(ctx, &req, &s, &diag);
    if (st != RKVC_OK) {
        fprintf(stderr, "%s: session create failed: %s\n", tag,
                rkvc_status_str(st));
        print_diag(diag);
        rkvc_context_destroy(ctx);
        return 2;
    }
    st = rkvc_session_start(s, &diag);
    if (st != RKVC_OK) {
        fprintf(stderr, "%s: session start failed: %s\n", tag,
                rkvc_status_str(st));
        print_diag(diag);
        rkvc_session_destroy(s);
        rkvc_context_destroy(ctx);
        return 2;
    }
    st = rkvc_session_wait(s);
    if (st != RKVC_OK) {
        char err[2048];
        rkvc_session_error_text(s, err, sizeof(err));
        fprintf(stderr, "%s: pipeline ended: %s\n%s", tag, rkvc_status_str(st),
                err);
    }
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
    if (cmd == "decode")
        return cmd_decode(a);
    if (cmd == "encode")
        return cmd_encode(a);
    if (cmd == "upscale")
        return cmd_upscale(a);
    if (cmd == "version")
        return cmd_version(a);
    if (cmd == "inspect")
        return cmd_inspect(a);
    cli::usage();
    return 1;
}
