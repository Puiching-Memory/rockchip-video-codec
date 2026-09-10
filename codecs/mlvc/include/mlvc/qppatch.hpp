// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace mlvc {

// QPP1 delta patch: applies a fine-tuned model's byte ranges onto the base
// model buffer in place (layout matches tools/mlvc/qppatch.py).
rkvc::Result<void> apply_qppatch(uint8_t* base, size_t base_size,
                                 const uint8_t* patch, size_t patch_size,
                                 int expected_qp /* <0 skips the check */,
                                 rkvc::Diag* diag = nullptr);

// CRC-32 (IEEE 802.3, zlib-compatible), exposed for tooling and tests.
uint32_t crc32(const uint8_t* data, size_t size) noexcept;

}  // namespace mlvc
