// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#include "rkvc/status.hpp"

namespace sr {
namespace post {

// Phase-RLFN CPU pre/post kernels (pure, no NPU types).
inline constexpr uint32_t kPhaseInFactor = 2;
inline constexpr uint32_t kPhaseOutFactor = 6;
inline constexpr uint32_t kPhaseInCh = 12;
inline constexpr uint32_t kPhaseOutCh = 108;
inline constexpr uint32_t kMaxDim = 8192;

// NV12 (w x h, both even, w/h multiples of 2) -> 12-channel phase packing.
rkvc::Status phase_pack_nv12(const uint8_t* y, uint32_t y_stride,
                              const uint8_t* uv, uint32_t uv_stride,
                              uint32_t w, uint32_t h, uint8_t* phases,
                              size_t phases_size) noexcept;
// Bicubic NV12 scaling (even dims, strides >= widths).
rkvc::Status bicubic_nv12(const uint8_t* src, uint32_t sw, uint32_t sh,
                           uint32_t s_stride, uint32_t s_vstride,
                           uint8_t* dst, uint32_t dw, uint32_t dh) noexcept;
// Add the 108-channel phase residual (rw x rh core) onto w x h NV12
// (w == rw*6, h == rh*6).
rkvc::Status add_phase_residual(const float* residual, uint32_t rw,
                                uint32_t rh, uint8_t* dst, uint32_t w,
                                uint32_t h) noexcept;

}  // namespace post
}  // namespace sr
