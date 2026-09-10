// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

enum class PixelFormat : uint32_t {
    Unknown = 0,
    Nv12,
    Nv21,
    Yuv420P,
    Nv16,
    P010,
    Rgb24,
    Bitstream,
};

enum class MemDomain : uint32_t {
    Host = 0,
    Dmabuf,
};

/// Negotiation object between ports. Zero width/height/stride means wildcard.
struct Spec {
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat fmt = PixelFormat::Unknown;
    MemDomain domain = MemDomain::Host;
    uint32_t stride = 0;
    uint32_t ver_stride = 0;
    uint64_t modifier = 0;
};

constexpr int64_t kTsUnknown = INT64_MIN;

bool is_linear(const Spec& s) noexcept;
uint32_t eff_stride(const Spec& s) noexcept;
uint32_t eff_ver_stride(const Spec& s) noexcept;
/// Minimum host bytes for the spec; 0 means unbounded (probe/bitstream).
size_t min_size(const Spec& s) noexcept;
const char* to_string(PixelFormat f) noexcept;

/// Field-level wildcard merge; conflict yields Negotiate with diag context.
Result<Spec> unify(const Spec& a, const Spec& b, Diag* diag = nullptr);

}  // namespace rkvc
