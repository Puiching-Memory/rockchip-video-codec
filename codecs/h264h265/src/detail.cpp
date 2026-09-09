// SPDX-License-Identifier: AGPL-3.0-or-later
#include "h264h265/detail.hpp"

namespace h264h265 {
namespace detail {

SniffedCodec sniff_annexb(const uint8_t* data, size_t size) noexcept {
    if (!data)
        return SniffedCodec::Unknown;
    size_t i = 0;
    while (i + 4 < size) {
        size_t hdr = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            hdr = 3;
        } else if (i + 5 <= size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            hdr = 4;
        } else {
            ++i;
            continue;
        }
        uint8_t nal = data[i + hdr];
        if ((nal & 0x1f) == 7)
            return SniffedCodec::Avc;
        if (((nal >> 1) & 0x3f) == 32)
            return SniffedCodec::Hevc;
        i += hdr;
    }
    return SniffedCodec::Unknown;
}

rkvc::Result<std::vector<ClampedRoi>> clamp_roi(
    const rkvc::RoiRegion* regions, size_t count, uint32_t frame_w,
    uint32_t frame_h, rkvc::Diag* diag) {
    using R = rkvc::Result<std::vector<ClampedRoi>>;
    if (count > rkvc::kRoiMaxRegions) {
        if (diag)
            diag->add("roi", "clamp", "too many regions");
        return R::failure(rkvc::Status::Invalid, diag ? *diag : rkvc::Diag{});
    }
    if (count && !regions) {
        if (diag)
            diag->add("roi", "clamp", "null regions");
        return R::failure(rkvc::Status::Invalid, diag ? *diag : rkvc::Diag{});
    }
    std::vector<ClampedRoi> out;
    for (size_t i = 0; i < count; ++i) {
        const rkvc::RoiRegion& src = regions[i];
        uint32_t left = src.x & ~15u;
        uint32_t top = src.y & ~15u;
        uint64_t right = (static_cast<uint64_t>(src.x) + src.width + 15u) &
                         ~static_cast<uint64_t>(15u);
        uint64_t bottom = (static_cast<uint64_t>(src.y) + src.height + 15u) &
                          ~static_cast<uint64_t>(15u);
        if (right > frame_w)
            right = frame_w;
        if (bottom > frame_h)
            bottom = frame_h;
        auto bad = [&] {
            if (diag)
                diag->add("roi", "clamp", "region out of frame");
            return R::failure(rkvc::Status::Format,
                              diag ? *diag : rkvc::Diag{});
        };
        if (left > UINT16_MAX || top > UINT16_MAX || right <= left ||
            bottom <= top || right - left > UINT16_MAX ||
            bottom - top > UINT16_MAX)
            return bad();
        ClampedRoi r;
        r.x = static_cast<uint16_t>(left);
        r.y = static_cast<uint16_t>(top);
        r.w = static_cast<uint16_t>(right - left);
        r.h = static_cast<uint16_t>(bottom - top);
        r.qp_delta = src.qp_delta;
        r.force_intra = src.force_intra;
        out.push_back(r);
    }
    return R::success(std::move(out));
}

}  // namespace detail
}  // namespace h264h265
