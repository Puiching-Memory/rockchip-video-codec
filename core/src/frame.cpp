// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/frame.hpp"

#include <new>

namespace rkvc {

Frame::~Frame() {
    if (hooks_.release)
        hooks_.release(hooks_.ctx);
}

Result<std::shared_ptr<Frame>> Frame::borrow_host(const Spec& spec, void* data,
                                                  size_t size, FrameHooks hooks,
                                                  Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("wrap", "frame", reason);
        return Result<std::shared_ptr<Frame>>::failure(s,
                                                       diag ? *diag : Diag{});
    };
    if (spec.domain != MemDomain::Host)
        return reject(Status::Invalid, "host borrow needs host domain");
    if (spec.fmt != PixelFormat::Unknown) {
        if (!data)
            return reject(Status::Invalid, "null host payload");
        size_t need = min_size(spec);
        if (need && size < need)
            return reject(Status::Invalid, "host payload too small");
    }
    std::shared_ptr<Frame> f(new (std::nothrow)
                                 Frame(spec, data, size, -1, hooks));
    if (!f)
        return reject(Status::Nomem, "frame alloc failed");
    return Result<std::shared_ptr<Frame>>::success(std::move(f));
}

Result<std::shared_ptr<Frame>> Frame::borrow_dmabuf(const Spec& spec, int fd,
                                                    size_t size,
                                                    FrameHooks hooks,
                                                    Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("wrap", "frame", reason);
        return Result<std::shared_ptr<Frame>>::failure(s,
                                                       diag ? *diag : Diag{});
    };
    if (spec.domain != MemDomain::Dmabuf)
        return reject(Status::Invalid, "dmabuf borrow needs dmabuf domain");
    if (fd < 0)
        return reject(Status::Invalid, "bad dmabuf fd");
    if (!is_linear(spec))
        return reject(Status::Format, "non-linear modifier rejected");
    std::shared_ptr<Frame> f(new (std::nothrow)
                                 Frame(spec, nullptr, size, fd, hooks));
    if (!f)
        return reject(Status::Nomem, "frame alloc failed");
    return Result<std::shared_ptr<Frame>>::success(std::move(f));
}

Result<void> Frame::set_roi(const RoiRegion* regions, size_t count,
                            Diag* diag) {
    auto reject = [&](const char* reason) {
        if (diag)
            diag->add("wrap", "roi", reason);
        return Result<void>::failure(Status::Invalid, diag ? *diag : Diag{});
    };
    if (count > kRoiMaxRegions)
        return reject("too many roi regions");
    if (count && !regions)
        return reject("null roi regions");
    for (size_t i = 0; i < count; ++i) {
        if (regions[i].qp_delta < -51 || regions[i].qp_delta > 51)
            return reject("roi qp_delta out of range");
        if (spec_.width && spec_.height) {
            if (regions[i].x + regions[i].width > spec_.width ||
                regions[i].y + regions[i].height > spec_.height)
                return reject("roi region out of frame");
        }
        roi_[i] = regions[i];
    }
    roi_count_ = count;
    return Result<void>::success();
}

}  // namespace rkvc
