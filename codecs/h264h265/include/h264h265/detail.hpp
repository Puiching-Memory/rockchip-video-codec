// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/frame.hpp"
#include "rkvc/result.hpp"

namespace h264h265 {
namespace detail {

// MPP-free pure logic, unit-tested without the MPP library.
enum class SniffedCodec : uint32_t {
    Unknown = 0,
    Avc,
    Hevc,
};

// Annex-B parameter-set sniffing (H.264 SPS / HEVC VPS). AV1 has no
// start codes and stays Unknown (caller must pass an explicit codec).
SniffedCodec sniff_annexb(const uint8_t* data, size_t size) noexcept;

struct ClampedRoi {
    uint16_t x = 0;
    uint16_t y = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    int16_t qp_delta = 0;
    bool force_intra = false;
};

// Align to 16px, clip into the frame. Invalid rects yield Invalid/Format.
rkvc::Result<std::vector<ClampedRoi>> clamp_roi(const rkvc::RoiRegion* regions,
                                                size_t count,
                                                uint32_t frame_w,
                                                uint32_t frame_h,
                                                rkvc::Diag* diag = nullptr);

// /dev/mpp_service presence plus AVC codec support (defined in decoder.cpp).
bool mpp_device_present() noexcept;

}  // namespace detail
}  // namespace h264h265
