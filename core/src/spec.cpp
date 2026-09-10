// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/spec.hpp"

namespace rkvc {

bool is_linear(const Spec& s) noexcept {
    return s.modifier == 0;
}

static uint32_t bpp(const Spec& s) noexcept {
    switch (s.fmt) {
        case PixelFormat::P010:
            return 2;
        case PixelFormat::Rgb24:
            return 3;
        default:
            return 1;
    }
}

uint32_t eff_stride(const Spec& s) noexcept {
    if (s.stride)
        return s.stride;
    switch (s.fmt) {
        case PixelFormat::Yuv420P:
        case PixelFormat::Nv12:
        case PixelFormat::Nv21:
        case PixelFormat::Nv16:
            return s.width;
        case PixelFormat::P010:
            return s.width * 2;
        case PixelFormat::Rgb24:
            return s.width * 3;
        default:
            return 0;
    }
}

uint32_t eff_ver_stride(const Spec& s) noexcept {
    return s.ver_stride ? s.ver_stride : s.height;
}

size_t min_size(const Spec& s) noexcept {
    if (s.fmt == PixelFormat::Unknown || s.fmt == PixelFormat::Bitstream)
        return 0;
    uint32_t st = eff_stride(s);
    uint32_t vs = eff_ver_stride(s);
    if (st == 0 || vs == 0)
        return 0;
    uint64_t plane = static_cast<uint64_t>(st) * vs;
    switch (s.fmt) {
        case PixelFormat::Nv12:
        case PixelFormat::Nv21:
        case PixelFormat::Yuv420P:
        case PixelFormat::P010:
            return static_cast<size_t>(plane * 3 / 2);
        case PixelFormat::Nv16:
            return static_cast<size_t>(plane * 2);
        case PixelFormat::Rgb24:
            (void)bpp(s);
            return static_cast<size_t>(plane);
        default:
            return 0;
    }
}

const char* to_string(PixelFormat f) noexcept {
    switch (f) {
        case PixelFormat::Unknown:
            return "unknown";
        case PixelFormat::Nv12:
            return "nv12";
        case PixelFormat::Nv21:
            return "nv21";
        case PixelFormat::Yuv420P:
            return "yuv420p";
        case PixelFormat::Nv16:
            return "nv16";
        case PixelFormat::P010:
            return "p010";
        case PixelFormat::Rgb24:
            return "rgb24";
        case PixelFormat::Bitstream:
            return "bitstream";
    }
    return "unknown";
}

static bool merge_dim(uint32_t a, uint32_t b, uint32_t& out) noexcept {
    if (a && b && a != b)
        return false;
    out = a ? a : b;
    return true;
}

Result<Spec> unify(const Spec& a, const Spec& b, Diag* diag) {
    if (a.fmt == PixelFormat::Unknown)
        return Result<Spec>::success(b);
    if (b.fmt == PixelFormat::Unknown)
        return Result<Spec>::success(a);
    Spec r = a;
    auto reject = [&](const char* reason) {
        if (diag)
            diag->add("negotiate", "spec", reason);
        return Result<Spec>::failure(Status::Negotiate, diag ? *diag : Diag{});
    };
    if (a.fmt != b.fmt)
        return reject("pixel format mismatch");
    if (a.domain != b.domain)
        return reject("memory domain mismatch");
    if (a.modifier != b.modifier)
        return reject("modifier mismatch");
    if (!merge_dim(a.width, b.width, r.width) ||
        !merge_dim(a.height, b.height, r.height) ||
        !merge_dim(a.stride, b.stride, r.stride) ||
        !merge_dim(a.ver_stride, b.ver_stride, r.ver_stride))
        return reject("dimension mismatch");
    return Result<Spec>::success(r);
}

}  // namespace rkvc
