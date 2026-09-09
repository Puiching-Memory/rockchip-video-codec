// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"
#include "rkvc/status.hpp"

namespace mlvc {

// Host-copy NPU model interface (v1; native zero-copy is a later
// optimization). All tensors are fp16 little-endian element arrays:
// inputs NHWC, outputs logical NCHW. No exceptions cross implementations.
struct NpuTensorInfo {
    std::string name;
    std::vector<uint32_t> dims;
};

class NpuModel {
public:
    virtual ~NpuModel() = default;
    // patch may be empty (no QPPATCH rung); patch_qp < 0 skips the check.
    virtual rkvc::Status init(std::span<const uint8_t> model,
                              std::span<const uint8_t> patch, int patch_qp,
                              rkvc::Diag* diag) = 0;
    virtual std::vector<NpuTensorInfo> inputs() const = 0;
    virtual std::vector<NpuTensorInfo> outputs() const = 0;
    virtual rkvc::Status set_input(size_t idx,
                                   std::span<const uint16_t> fp16) = 0;
    virtual rkvc::Status run(rkvc::Diag* diag) = 0;
    // Valid until the next run() or set_input().
    virtual std::span<const uint16_t> output(size_t idx) const = 0;
};

using NpuModelFn = std::function<std::unique_ptr<NpuModel>()>;

// Fake model for container tests: fixed small geometry, deterministic
// invertible stub math (see npu_fake.cpp). Never ships in the plugin.
struct FakeNpuGeometry {
    uint32_t img = 64;
    uint32_t y_c = 8;
    uint32_t y_hw = 16;
    uint32_t z_c = 8;
    uint32_t z_hw = 4;
    uint32_t ref_c = 4;
    uint32_t ref_hw = 8;
};

std::unique_ptr<NpuModel> make_fake_encoder(const FakeNpuGeometry& g = {});
std::unique_ptr<NpuModel> make_fake_decoder(const FakeNpuGeometry& g = {});

// Real RKNN backend; null when built without librknnrt.
std::unique_ptr<NpuModel> make_rknn_model();

}  // namespace mlvc
