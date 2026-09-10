// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/node.hpp"
#include "rkvc/registry.hpp"
#include "rkvc/result.hpp"

namespace sr {

// Phase-RLFN upscaler ("rknn.upscale", Transform stage): fixed 3x NV12
// upscale = bicubic baseline + NPU phase residual. The NPU backend is
// injected (real RKNN in the plugin, absent without the runtime).
struct SrGeometry {
    uint32_t core_w = 0;  // model core pixels (input = 2x, output = 6x)
    uint32_t core_h = 0;
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    uint32_t out_w = 0;
    uint32_t out_h = 0;
};

class SrRuntime {
public:
    virtual ~SrRuntime() = default;
    // Validates the 12ch-u8-in / 108ch-f32-out contract and reports geometry.
    virtual rkvc::Status open(std::span<const uint8_t> model, SrGeometry& g,
                              rkvc::Diag* diag) = 0;
    virtual rkvc::Status run(std::span<const uint8_t> packed12,
                             std::span<float> residual108,
                             rkvc::Diag* diag) = 0;
};

std::unique_ptr<SrRuntime> make_rknn_runtime();  // null without librknnrt

class SrUpscaleNode : public rkvc::Node {
public:
    struct Impl;
    SrUpscaleNode(rkvc::Request req, std::unique_ptr<SrRuntime> rt);
    ~SrUpscaleNode() override;
    std::string_view id() const noexcept override { return "rknn.upscale"; }
    std::vector<rkvc::Port> make_ports() const override;
    rkvc::Status configure(std::vector<rkvc::Port>& ports,
                           rkvc::Diag* diag) override;
    rkvc::Status open(rkvc::Emit* emit, rkvc::Diag* diag) override;
    rkvc::Status process(rkvc::FramePtr input, rkvc::Diag* diag) override;
    void close() noexcept override;
    bool wants_model() const noexcept override { return true; }
    rkvc::Status bind_model(const rkvc::Model& model,
                            rkvc::Diag* diag) override;

private:
    std::unique_ptr<Impl> impl_;
};

class SrUpscaleFactory : public rkvc::Factory {
public:
    std::string_view id() const noexcept override { return "rknn.upscale"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Transform;
    }
    int priority() const noexcept override { return 100; }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override {
        // No device gate here: without an NPU the session still builds
        // and fails cleanly at open (Hw, fallback-eligible), which keeps
        // the bind path testable where no NPU exists.
        return r.operation == rkvc::Operation::Upscale;
    }
    int score(const rkvc::Request& r,
              const rkvc::DeviceCaps&) const noexcept override {
        return r.policy == rkvc::Policy::Realtime ? 100 : 220;
    }
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request& r,
                                       rkvc::Diag* diag) const override;

private:
    static bool npu_present() noexcept;
};

}  // namespace sr
