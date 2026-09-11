// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/rkvc.h"

#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>
#endif

#include "rkvc/context.hpp"
#include "rkvc/model.hpp"
#include "rkvc/session.hpp"

struct rkvc_context {
    explicit rkvc_context(rkvc::ContextOptions opts = {})
        : ctx(std::move(opts)) {}
    rkvc::Context ctx;
};

struct rkvc_session {
    std::shared_ptr<rkvc::Session> s;
};

struct rkvc_frame {
    rkvc::FramePtr f;
};

struct rkvc_diagnostic {
    rkvc_status status = RKVC_OK;
    rkvc::Diag diag;
};

namespace {

static_assert((int)RKVC_OK == (int)rkvc::Status::Ok);
static_assert((int)RKVC_NOMEM == (int)rkvc::Status::Nomem);
static_assert((int)RKVC_INVALID == (int)rkvc::Status::Invalid);
static_assert((int)RKVC_NOT_FOUND == (int)rkvc::Status::NotFound);
static_assert((int)RKVC_IO == (int)rkvc::Status::Io);
static_assert((int)RKVC_HW == (int)rkvc::Status::Hw);
static_assert((int)RKVC_EOF == (int)rkvc::Status::Eof);
static_assert((int)RKVC_AGAIN == (int)rkvc::Status::Again);
static_assert((int)RKVC_FORMAT == (int)rkvc::Status::Format);
static_assert((int)RKVC_NEGOTIATE == (int)rkvc::Status::Negotiate);
static_assert((int)RKVC_PERMISSION == (int)rkvc::Status::Permission);
static_assert((int)RKVC_CANCELED == (int)rkvc::Status::Canceled);
static_assert((int)RKVC_UNSUPPORTED == (int)rkvc::Status::Unsupported);
static_assert((int)RKVC_INTERNAL == (int)rkvc::Status::Internal);
static_assert((int)RKVC_MODEL == (int)rkvc::Status::Model);
static_assert((int)RKVC_LICENSE == (int)rkvc::Status::License);
static_assert((int)RKVC_INTEGRITY == (int)rkvc::Status::Integrity);

rkvc::Status cs(rkvc_status s) {
    return static_cast<rkvc::Status>((int)s);
}
rkvc_status sc(rkvc::Status s) {
    return static_cast<rkvc_status>((int)s);
}

bool check_struct(size_t size, uint32_t version, size_t want) {
    return size >= want && version == RKVC_ABI_VERSION;
}

rkvc::PixelFormat map_fmt(rkvc_frame_fmt f) {
    switch (f) {
        case RKVC_FRAME_FMT_NV12:
            return rkvc::PixelFormat::Nv12;
        case RKVC_FRAME_FMT_NV21:
            return rkvc::PixelFormat::Nv21;
        case RKVC_FRAME_FMT_YUV420P:
            return rkvc::PixelFormat::Yuv420P;
        case RKVC_FRAME_FMT_NV16:
            return rkvc::PixelFormat::Nv16;
        case RKVC_FRAME_FMT_P010:
            return rkvc::PixelFormat::P010;
        case RKVC_FRAME_FMT_RGB24:
            return rkvc::PixelFormat::Rgb24;
        case RKVC_FRAME_FMT_BITSTREAM:
            return rkvc::PixelFormat::Bitstream;
        default:
            return rkvc::PixelFormat::Unknown;
    }
}

rkvc_frame_fmt unmap_fmt(rkvc::PixelFormat f) {
    switch (f) {
        case rkvc::PixelFormat::Nv12:
            return RKVC_FRAME_FMT_NV12;
        case rkvc::PixelFormat::Nv21:
            return RKVC_FRAME_FMT_NV21;
        case rkvc::PixelFormat::Yuv420P:
            return RKVC_FRAME_FMT_YUV420P;
        case rkvc::PixelFormat::Nv16:
            return RKVC_FRAME_FMT_NV16;
        case rkvc::PixelFormat::P010:
            return RKVC_FRAME_FMT_P010;
        case rkvc::PixelFormat::Rgb24:
            return RKVC_FRAME_FMT_RGB24;
        case rkvc::PixelFormat::Bitstream:
            return RKVC_FRAME_FMT_BITSTREAM;
        default:
            return RKVC_FRAME_FMT_UNKNOWN;
    }
}

rkvc::Spec map_spec(const rkvc_frame_spec& s) {
    rkvc::Spec o;
    o.width = s.width;
    o.height = s.height;
    o.fmt = map_fmt(s.fmt);
    o.domain = (s.domain == RKVC_MEM_DOMAIN_DMABUF) ? rkvc::MemDomain::Dmabuf
                                                    : rkvc::MemDomain::Host;
    o.stride = s.stride;
    o.ver_stride = s.ver_stride;
    o.modifier = s.modifier;
    return o;
}

rkvc_frame_spec unmap_spec(const rkvc::Spec& s) {
    rkvc_frame_spec o = {};
    o.width = s.width;
    o.height = s.height;
    o.fmt = unmap_fmt(s.fmt);
    o.domain = (s.domain == rkvc::MemDomain::Dmabuf) ? RKVC_MEM_DOMAIN_DMABUF
                                                     : RKVC_MEM_DOMAIN_HOST;
    o.stride = s.stride;
    o.ver_stride = s.ver_stride;
    o.modifier = s.modifier;
    return o;
}

rkvc_diagnostic* make_diag(rkvc::Status s, rkvc::Diag d) {
    rkvc_diagnostic* out = new (std::nothrow) rkvc_diagnostic();
    if (!out)
        return nullptr;
    out->status = sc(s);
    out->diag = std::move(d);
    return out;
}

#ifdef __linux__
void discover_dir(rkvc::Context& ctx, const char* dir) {
    if (!dir || !*dir)
        return;
    DIR* dp = opendir(dir);
    if (!dp)
        return;
    while (dirent* e = readdir(dp)) {
        std::string name = e->d_name;
        if (name.size() < 4 || name.compare(name.size() - 3, 3, ".so") != 0)
            continue;
        ctx.load_plugin(std::string(dir) + "/" + name);
    }
    closedir(dp);
}
#endif

}  // namespace

const char* rkvc_status_str(rkvc_status status) {
    return rkvc::to_string(cs(status));
}

void rkvc_context_options_init(rkvc_context_options* opts, size_t size) {
    if (!opts || size < sizeof(*opts))
        return;
    memset(opts, 0, sizeof(*opts));
    opts->struct_size = sizeof(*opts);
    opts->version = RKVC_ABI_VERSION;
}

rkvc_status rkvc_context_create(const rkvc_context_options* opts,
                                rkvc_context** out) {
    if (!out)
        return RKVC_INVALID;
    *out = nullptr;
    rkvc::ContextOptions copts;
    if (opts) {
        if (!check_struct(opts->struct_size, opts->version, sizeof(*opts)))
            return RKVC_INVALID;
        for (size_t i = 0; i < opts->backend_dir_count; ++i)
            if (opts->backend_dirs && opts->backend_dirs[i])
                copts.backend_dirs.emplace_back(opts->backend_dirs[i]);
    }
    rkvc_context* ctx = new (std::nothrow) rkvc_context(std::move(copts));
    if (!ctx)
        return RKVC_NOMEM;
#ifdef __linux__
    // 注意：上面的 copts 已被移动走，这里从调用者的原始副本重新扫描。
    if (opts) {
        for (size_t i = 0; i < opts->backend_dir_count; ++i)
            if (opts->backend_dirs && opts->backend_dirs[i])
                discover_dir(ctx->ctx, opts->backend_dirs[i]);
    }
    Dl_info info = {};
    if (dladdr((const void*)rkvc_context_create, &info) && info.dli_fname) {
        std::string self = info.dli_fname;
        size_t slash = self.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = self.substr(0, slash);
            // 优先同目录布局，其次是可移植包布局
            //（bin/rkvc 可执行文件，插件位于 lib/rkvc/backends）。
            discover_dir(ctx->ctx, (dir + "/rkvc/backends").c_str());
            discover_dir(ctx->ctx, (dir + "/../lib/rkvc/backends").c_str());
        }
    }
    discover_dir(ctx->ctx, "/usr/local/lib/rkvc/backends");
    discover_dir(ctx->ctx, "/usr/lib/rkvc/backends");
#endif
    *out = ctx;
    return RKVC_OK;
}

void rkvc_context_destroy(rkvc_context* ctx) {
    delete ctx;
}

rkvc_status rkvc_context_add_model_dir(rkvc_context* ctx, const char* dir,
                                       rkvc_diagnostic** diag) {
    if (diag)
        *diag = nullptr;
    if (!ctx || !dir || !*dir)
        return RKVC_INVALID;
    rkvc::Diag d;
    auto fail = [&](rkvc::Status s, const char* reason) {
        d.add("load", "model", reason);
        if (diag)
            *diag = make_diag(s, std::move(d));
        return sc(s);
    };
    auto ms = rkvc::load_model_dir(dir, &d);
    if (!ms) {
        if (diag)
            *diag = make_diag(ms.status(), std::move(d));
        return sc(ms.status());
    }
    for (auto& m : ms.value()) {
        rkvc::Status st = ctx->ctx.add_model(std::move(m));
        if (st != rkvc::Status::Ok)
            return fail(st, "register failed");
    }
    return RKVC_OK;
}

rkvc_status rkvc_probe_device(rkvc_context* ctx, rkvc_caps* caps) {
    if (!ctx || !caps)
        return RKVC_INVALID;
    if (!check_struct(caps->struct_size, caps->version, sizeof(*caps)))
        return RKVC_INVALID;
    rkvc::DeviceCaps dc = ctx->ctx.probe_device();
    memset(caps->soc, 0, sizeof(caps->soc));
    strncpy(caps->soc, dc.soc.c_str(), sizeof(caps->soc) - 1);
    caps->has_mpp_encoder = dc.mpp_encode ? 1 : 0;
    caps->has_mpp_decoder = dc.mpp_decode ? 1 : 0;
    caps->has_rknn = dc.rknn ? 1 : 0;
    caps->npu_cores = dc.npu_cores;
    return RKVC_OK;
}

void rkvc_session_request_init(rkvc_session_request* req, size_t size) {
    if (!req || size < sizeof(*req))
        return;
    memset(req, 0, sizeof(*req));
    req->struct_size = sizeof(*req);
    req->version = RKVC_ABI_VERSION;
    req->quality.qp = -1;  // 自动；0 表示固定 QP 0
    req->queue_capacity = 4;
}

namespace {

bool map_request(const rkvc_session_request& in, rkvc::Request& out) {
    switch (in.operation) {
        case RKVC_OP_TRANSCODE:
            out.operation = rkvc::Operation::Transcode;
            break;
        case RKVC_OP_DECODE:
            out.operation = rkvc::Operation::Decode;
            break;
        case RKVC_OP_ENCODE:
            out.operation = rkvc::Operation::Encode;
            break;
        case RKVC_OP_UPSCALE:
            out.operation = rkvc::Operation::Upscale;
            break;
        default:
            return false;
    }
    switch (in.codec) {
        case RKVC_CODEC_AUTO:
            out.codec = rkvc::Codec::Auto;
            break;
        case RKVC_CODEC_H264:
            out.codec = rkvc::Codec::H264;
            break;
        case RKVC_CODEC_HEVC:
            out.codec = rkvc::Codec::Hevc;
            break;
        case RKVC_CODEC_AV1:
            out.codec = rkvc::Codec::Av1;
            break;
        case RKVC_CODEC_MLVC:
            out.codec = rkvc::Codec::Mlvc;
            break;
        default:
            return false;
    }
    switch (in.policy) {
        case RKVC_POLICY_REALTIME:
            out.policy = rkvc::Policy::Realtime;
            break;
        case RKVC_POLICY_BALANCED:
            out.policy = rkvc::Policy::Balanced;
            break;
        case RKVC_POLICY_QUALITY:
            out.policy = rkvc::Policy::Quality;
            break;
        case RKVC_POLICY_OFFLINE:
            out.policy = rkvc::Policy::Offline;
            break;
        default:
            return false;
    }
    auto map_endpoint = [](const rkvc_endpoint& e, rkvc::Endpoint& o,
                           rkvc::Spec& spec) {
        switch (e.kind) {
            case RKVC_ENDPOINT_FILE:
                o.kind = rkvc::EndpointKind::File;
                break;
            case RKVC_ENDPOINT_FRAME_SINK:
                o.kind = rkvc::EndpointKind::FrameSink;
                break;
            default:
                o.kind = rkvc::EndpointKind::Stream;
                break;
        }
        if (e.uri)
            o.uri = e.uri;
        spec.fmt = map_fmt(e.fmt);
        spec.width = e.width;
        spec.height = e.height;
    };
    map_endpoint(in.input, out.input, out.input_spec);
    map_endpoint(in.output, out.output, out.output_spec);
    out.quality.bitrate_bps = in.quality.bitrate_bps;
    out.quality.qp = in.quality.qp;
    out.quality.gop_size = in.quality.gop_size;
    out.quality.fps = in.quality.fps;
    if (in.model_id)
        out.model_id = in.model_id;
    out.queue_capacity = in.queue_capacity ? in.queue_capacity : 4;
    return true;
}

}  // namespace

rkvc_status rkvc_session_create(rkvc_context* ctx,
                                const rkvc_session_request* req,
                                rkvc_session** out, rkvc_diagnostic** diag) {
    if (diag)
        *diag = nullptr;
    if (!ctx || !req || !out)
        return RKVC_INVALID;
    *out = nullptr;
    if (!check_struct(req->struct_size, req->version, sizeof(*req)))
        return RKVC_INVALID;
    rkvc::Request r;
    if (!map_request(*req, r))
        return RKVC_INVALID;
    rkvc::Diag d;
    auto s = rkvc::Session::create(ctx->ctx, r, &d);
    if (!s) {
        if (diag)
            *diag = make_diag(s.status(), std::move(d));
        return sc(s.status());
    }
    rkvc_session* wrap = new (std::nothrow) rkvc_session();
    if (!wrap)
        return RKVC_NOMEM;
    wrap->s = std::move(s.value());
    *out = wrap;
    return RKVC_OK;
}

rkvc_status rkvc_session_start(rkvc_session* s, rkvc_diagnostic** diag) {
    if (diag)
        *diag = nullptr;
    if (!s)
        return RKVC_INVALID;
    rkvc::Diag d;
    rkvc::Status st = s->s->start(&d);
    if (st != rkvc::Status::Ok && diag)
        *diag = make_diag(st, std::move(d));
    return sc(st);
}

rkvc_status rkvc_session_push(rkvc_session* s, rkvc_frame* f) {
    if (!s || !f || !f->f)
        return RKVC_INVALID;
    return sc(s->s->push(f->f));
}

rkvc_status rkvc_session_try_pull(rkvc_session* s, rkvc_frame** out) {
    if (!s || !out)
        return RKVC_INVALID;
    *out = nullptr;
    auto r = s->s->try_pull();
    if (!r)
        return sc(r.status());
    rkvc_frame* wrap = new (std::nothrow) rkvc_frame();
    if (!wrap)
        return RKVC_NOMEM;
    wrap->f = std::move(r.value());
    *out = wrap;
    return RKVC_OK;
}

rkvc_status rkvc_session_pull(rkvc_session* s, rkvc_frame** out) {
    if (!s || !out)
        return RKVC_INVALID;
    *out = nullptr;
    auto r = s->s->pull();
    if (!r)
        return sc(r.status());
    rkvc_frame* wrap = new (std::nothrow) rkvc_frame();
    if (!wrap)
        return RKVC_NOMEM;
    wrap->f = std::move(r.value());
    *out = wrap;
    return RKVC_OK;
}

rkvc_status rkvc_session_push_eos(rkvc_session* s) {
    if (!s)
        return RKVC_INVALID;
    return sc(s->s->push_eos());
}

rkvc_status rkvc_session_wait(rkvc_session* s) {
    if (!s)
        return RKVC_INVALID;
    return sc(s->s->wait());
}

rkvc_status rkvc_session_error_text(rkvc_session* s, char* buf, size_t size) {
    if (!s)
        return RKVC_INVALID;
    if (buf && size) {
        std::string text = s->s->error_diag().format();
        size_t n = text.size() < size - 1 ? text.size() : size - 1;
        memcpy(buf, text.data(), n);
        buf[n] = '\0';
    }
    return sc(s->s->wait());
}

void rkvc_session_destroy(rkvc_session* s) {
    delete s;
}

void rkvc_frame_desc_init(rkvc_frame_desc* desc, size_t size) {
    if (!desc || size < sizeof(*desc))
        return;
    memset(desc, 0, sizeof(*desc));
    desc->struct_size = sizeof(*desc);
    desc->version = RKVC_ABI_VERSION;
    desc->fd = -1;
    desc->pts = RKVC_FRAME_TS_UNKNOWN;
    desc->dts = RKVC_FRAME_TS_UNKNOWN;
}

rkvc_status rkvc_frame_wrap(const rkvc_frame_desc* desc, rkvc_frame** out) {
    if (!desc || !out)
        return RKVC_INVALID;
    *out = nullptr;
    if (!check_struct(desc->struct_size, desc->version, sizeof(*desc)))
        return RKVC_INVALID;
    rkvc::Spec spec = map_spec(desc->spec);
    rkvc::Result<rkvc::FramePtr> r =
        (spec.domain == rkvc::MemDomain::Dmabuf)
            ? rkvc::Frame::borrow_dmabuf(spec, desc->fd, desc->size)
            : rkvc::Frame::borrow_host(spec, desc->data, desc->size);
    if (!r)
        return sc(r.status());
    r.value()->set_pts(desc->pts);
    r.value()->set_dts(desc->dts);
    r.value()->set_flags(desc->flags);
    rkvc_frame* wrap = new (std::nothrow) rkvc_frame();
    if (!wrap)
        return RKVC_NOMEM;
    wrap->f = std::move(r.value());
    *out = wrap;
    return RKVC_OK;
}

rkvc_status rkvc_frame_wrap_owned(
    const rkvc_frame_desc* desc,
    void (*release)(void* release_ctx) noexcept, void* release_ctx,
    rkvc_frame** out) {
    if (!desc || !release || !out)
        return RKVC_INVALID;
    *out = nullptr;
    if (!check_struct(desc->struct_size, desc->version, sizeof(*desc)))
        return RKVC_INVALID;
    rkvc::Spec spec = map_spec(desc->spec);
    rkvc::FrameHooks hooks;
    hooks.release = release;
    hooks.ctx = release_ctx;
    rkvc::Result<rkvc::FramePtr> r =
        (spec.domain == rkvc::MemDomain::Dmabuf)
            ? rkvc::Frame::borrow_dmabuf(spec, desc->fd, desc->size, hooks)
            : rkvc::Frame::borrow_host(spec, desc->data, desc->size, hooks);
    if (!r)
        return sc(r.status());
    r.value()->set_pts(desc->pts);
    r.value()->set_dts(desc->dts);
    r.value()->set_flags(desc->flags);
    rkvc_frame* wrap = new (std::nothrow) rkvc_frame();
    if (!wrap)
        return RKVC_NOMEM;
    wrap->f = std::move(r.value());
    *out = wrap;
    return RKVC_OK;
}

rkvc_status rkvc_frame_get_desc(const rkvc_frame* f, rkvc_frame_desc* desc) {
    if (!f || !f->f || !desc)
        return RKVC_INVALID;
    if (!check_struct(desc->struct_size, desc->version, sizeof(*desc)))
        return RKVC_INVALID;
    rkvc_frame_spec spec = unmap_spec(f->f->spec());
    desc->spec = spec;
    desc->data = f->f->data();
    desc->size = f->f->size();
    desc->fd = f->f->fd();
    desc->pts = f->f->pts();
    desc->dts = f->f->dts();
    desc->flags = f->f->flags();
    return RKVC_OK;
}

void rkvc_frame_release(rkvc_frame* f) {
    delete f;
}

void rkvc_diag_fmt_text(const rkvc_diagnostic* diag, char* buf, size_t size) {
    if (!buf || !size)
        return;
    buf[0] = '\0';
    if (!diag)
        return;
    size_t used = 0;
    auto append = [&](const rkvc::DiagEntry& e) {
        int n = snprintf(buf + used, size - used, "status=%d %s(%s): %s\n",
                         (int)diag->status, e.stage.c_str(), e.subject.c_str(),
                         e.reason.c_str());
        if (n > 0)
            used += static_cast<size_t>(n) < size - used
                        ? static_cast<size_t>(n)
                        : size - used - 1;
    };
    for (size_t i = 0; i < diag->diag.size(); ++i)
        append(diag->diag.at(i));
}

void rkvc_diag_release(rkvc_diagnostic* diag) {
    delete diag;
}
