// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace mlvc {

// QPT1 per-QP FiLM row tables (layout matches tools/mlvc/qptab.py).
inline constexpr size_t kQptabMaxTables = 8;
inline constexpr size_t kQptabNameMax = 32;

struct QpTable {
    std::string name;
    uint32_t rows = 0;
    uint32_t cols = 0;
    std::vector<uint16_t> data;  // rows*cols fp16, row-major
};

struct QpTables {
    std::vector<QpTable> tables;
    const QpTable* find(const std::string& name) const noexcept;
};

// Row q clamped to the last row; nullptr when the table is empty.
const uint16_t* qptab_row(const QpTable& t, uint32_t q) noexcept;

rkvc::Result<QpTables> load_qptab(const uint8_t* data, size_t size,
                                  rkvc::Diag* diag = nullptr);

}  // namespace mlvc
