// SPDX-License-Identifier: AGPL-3.0-or-later
// No-RKNN build: the real backend is unavailable; sessions fail cleanly at
// bind with Unsupported instead of crashing on missing symbols.
#include "mlvc/npu.hpp"

namespace mlvc {

std::unique_ptr<NpuModel> make_rknn_model() {
    return nullptr;
}

}  // namespace mlvc
