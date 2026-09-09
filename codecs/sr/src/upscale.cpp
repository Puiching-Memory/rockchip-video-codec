// SPDX-License-Identifier: AGPL-3.0-or-later
#include "sr/upscale.hpp"

#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "sr/post.hpp"

#include "rkvc/rkmodel.hpp"

namespace sr {

struct SrUpscaleNode::Impl {
    rkvc::Request req;
    std::unique_ptr<SrRuntime> rt;
    std::vector<uint8_t> model_bytes;  // borrowed content, copied for RKNN
    SrGeometry geom;
    bool opened = false;
    std::vector<uint8_t> packed;
    std::vector<float> residual;
    rkvc::Emit* emit = nullptr;
};

SrUpscaleNode::SrUpscaleNode(rkvc::Request req, std::unique_ptr<SrRuntime> rt)
    : impl_(new (std::nothrow) Impl()) {
    if (impl_) {
        impl_->req = std::move(req);
        impl_->rt = std::move(rt);
    }
}

SrUpscaleNode::~SrUpscaleNode() = default;

std::vector<rkvc::Port> SrUpscaleNode::make_ports() const {
    rkvc::Port in, out;
    in.name = "video";
    in.is_input = true;
    out.name = "video";
    out.desired.fmt = rkvc::PixelFormat::Nv12;
    out.desired.domain = rkvc::MemDomain::Host;
    return {in, out};
}

rkvc::Status SrUpscaleNode::configure(std::vector<rkvc::Port>& ports,
                                      rkvc::Diag* diag) {
    return rkvc::Node::configure(ports, diag);
}

rkvc::Status SrUpscaleNode::bind_model(const rkvc::Model& model,
                                       rkvc::Diag* diag) {
    Impl* up = impl_.get();
    if (!up)
        return rkvc::Status::Nomem;
    if (model.meta.role != "upscale") {
        if (diag)
            diag->add("bind", "rknn.upscale", "model role is not upscale");
        return rkvc::Status::Format;
    }
    const rkvc::ModelPayload* rknn = model.find("rknn");
    if (!rknn || rknn->data.empty()) {
        if (diag)
            diag->add("bind", "rknn.upscale", "missing rknn payload");
        return rkvc::Status::Format;
    }
    up->model_bytes.assign(rknn->data.begin(), rknn->data.end());
    return rkvc::Status::Ok;
}

rkvc::Status SrUpscaleNode::open(rkvc::Emit* emit, rkvc::Diag* diag) {
    Impl* up = impl_.get();
    if (!up)
        return rkvc::Status::Nomem;
    if (!emit) {
        if (diag)
            diag->add("open", "rknn.upscale", "null emit");
        return rkvc::Status::Invalid;
    }
    if (up->model_bytes.empty()) {
        if (diag)
            diag->add("open", "rknn.upscale", "opened without a bound model");
        return rkvc::Status::Format;
    }
    if (!up->rt)
        return rkvc::Status::Hw;  // no runtime: participates in HW fallback
    rkvc::Status st = up->rt->open(up->model_bytes, up->geom, diag);
    if (st != rkvc::Status::Ok)
        return st;
    if ((up->req.width && up->req.width != up->geom.in_w) ||
        (up->req.height && up->req.height != up->geom.in_h)) {
        if (diag)
            diag->add("open", "rknn.upscale", "request geometry mismatch");
        return rkvc::Status::Format;
    }
    up->packed.assign((size_t)post::kPhaseInCh * up->geom.core_w *
                          up->geom.core_h,
                      0);
    up->residual.assign(
        (size_t)post::kPhaseOutCh * up->geom.core_w * up->geom.core_h, 0.0f);
    up->emit = emit;
    up->opened = true;
    return rkvc::Status::Ok;
}

rkvc::Status SrUpscaleNode::process(rkvc::FramePtr input, rkvc::Diag* diag) {
    Impl* up = impl_.get();
    if (!up || !up->opened)
        return rkvc::Status::Invalid;
    auto bad = [&](rkvc::Status s, const char* reason) {
        if (diag)
            diag->add("process", "rknn.upscale", reason);
        return s;
    };
    if (!input || input->spec().fmt != rkvc::PixelFormat::Nv12)
        return bad(rkvc::Status::Format, "NV12 input only");
    const rkvc::Spec& spec = input->spec();
    if (spec.width != up->geom.in_w || spec.height != up->geom.in_h)
        return bad(rkvc::Status::Format, "geometry mismatch");
    uint32_t stride = spec.stride ? spec.stride : spec.width;
    uint32_t vstride = spec.ver_stride ? spec.ver_stride : spec.height;
    if (stride < spec.width || vstride < spec.height ||
        input->size() < (size_t)stride * vstride * 3 / 2)
        return bad(rkvc::Status::Format, "short input");

    const uint8_t* base =
        static_cast<const uint8_t*>(input->data());
    void* mapped = nullptr;
#ifdef __linux__
    if (spec.domain == rkvc::MemDomain::Dmabuf) {
        if (input->fd() < 0 || spec.modifier != 0)
            return bad(rkvc::Status::Format, "linear dmabuf only");
        mapped = mmap(nullptr, input->size(), PROT_READ, MAP_SHARED,
                      input->fd(), 0);
        if (mapped == MAP_FAILED)
            return bad(rkvc::Status::Io, "dmabuf map failed");
        struct dma_buf_sync sync = {};
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ;
        (void)ioctl(input->fd(), DMA_BUF_IOCTL_SYNC, &sync);
        base = static_cast<const uint8_t*>(mapped);
    } else
#endif
    {
        if (!base)
            return bad(rkvc::Status::Format, "null payload");
    }
    auto unmap = [&] {
#ifdef __linux__
        if (mapped) {
            struct dma_buf_sync sync = {};
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            (void)ioctl(input->fd(), DMA_BUF_IOCTL_SYNC, &sync);
            munmap(mapped, input->size());
        }
#else
        (void)input;
#endif
    };

    size_t out_size = (size_t)up->geom.out_w * up->geom.out_h * 3 / 2;
    uint8_t* out = static_cast<uint8_t*>(malloc(out_size));
    if (!out) {
        unmap();
        return bad(rkvc::Status::Nomem, "no memory");
    }
    rkvc::Status st = post::phase_pack_nv12(
        base, stride, base + (size_t)stride * vstride, stride, up->geom.in_w,
        up->geom.in_h, up->packed.data(), up->packed.size());
    if (st == rkvc::Status::Ok)
        st = up->rt->run(up->packed, up->residual, diag);
    if (st == rkvc::Status::Ok)
        st = post::bicubic_nv12(base, up->geom.in_w, up->geom.in_h, stride,
                                vstride, out, up->geom.out_w, up->geom.out_h);
    if (st == rkvc::Status::Ok)
        st = post::add_phase_residual(up->residual.data(), up->geom.core_w,
                                      up->geom.core_h, out, up->geom.out_w,
                                      up->geom.out_h);
    unmap();
    if (st != rkvc::Status::Ok) {
        free(out);
        return bad(st, "upscale failed");
    }
    rkvc::Spec ospec;
    ospec.width = up->geom.out_w;
    ospec.height = up->geom.out_h;
    ospec.fmt = rkvc::PixelFormat::Nv12;
    ospec.stride = up->geom.out_w;
    ospec.ver_stride = up->geom.out_h;
    auto fr = rkvc::Frame::borrow_host(
        ospec, out, out_size,
        {[](void* p) noexcept { free(p); }, out});
    if (!fr) {
        free(out);
        return bad(fr.status(), "output frame alloc failed");
    }
    fr.value()->set_pts(input->pts());
    fr.value()->set_dts(input->dts());
    fr.value()->set_flags(input->flags());
    return up->emit->emit(0, fr.value());
}

void SrUpscaleNode::close() noexcept {
    Impl* up = impl_.get();
    if (!up)
        return;
    up->rt.reset();
    up->opened = false;
}

bool SrUpscaleFactory::npu_present() noexcept {
#ifdef __linux__
    if (access("/sys/kernel/debug/rknpu/version", R_OK) == 0)
        return true;
    if (access("/dev/rknpu", R_OK | W_OK) == 0)
        return true;
    if (access("/dev/rknn", R_OK | W_OK) == 0)
        return true;
#endif
    return false;
}

rkvc::Result<rkvc::NodePtr> SrUpscaleFactory::create(
    const rkvc::Request& r, rkvc::Diag*) const {
    rkvc::NodePtr n(
        new (std::nothrow) SrUpscaleNode(r, make_rknn_runtime()));
    if (!n)
        return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
    return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
}

}  // namespace sr
