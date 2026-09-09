// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace rkvc {

// Minimal SHA-256 (FIPS 180-4), pure computation, no allocation.
std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len) noexcept;

}  // namespace rkvc
