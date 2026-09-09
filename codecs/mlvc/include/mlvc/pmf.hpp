// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace mlvc {

// PMF1 entropy-table payload (layout matches tools/mlvc/pmf.py).
struct Pmf {
    std::vector<int32_t> lengths;
    std::vector<int32_t> offsets;
    std::vector<int32_t> table;
    double scale_min = 0;
    double scale_max = 0;
    uint32_t scale_levels = 0;
    uint32_t index_space = 0;
    uint32_t qp_num = 0;    // tag 2 (bitest)
    uint32_t channels = 0;  // tag 2 (bitest)
    bool gaussian = false;  // tag 1
};

rkvc::Result<Pmf> load_pmf(const uint8_t* data, size_t size,
                           rkvc::Diag* diag = nullptr);

}  // namespace mlvc
