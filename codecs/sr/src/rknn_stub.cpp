// SPDX-License-Identifier: AGPL-3.0-or-later
// No-RKNN build: sessions fail cleanly at open with Hw (HW fallback).
#include "sr/upscale.hpp"

namespace sr {

std::unique_ptr<SrRuntime> make_rknn_runtime() {
    return nullptr;
}

}  // namespace sr
