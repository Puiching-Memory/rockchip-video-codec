// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>

#include "rkvc/node.hpp"
#include "rkvc/registry.hpp"

namespace h264h265 {

// MPP hard decoder ("mpp.decode", DECODE stage). Emits linear DMABUF
// frames; explicit codec initializes at open, AUTO/TRANSCODE sniffs the
// first Annex-B frame (AV1 needs an explicit codec).
class MppDecoderNode : public rkvc::Node {
public:
    struct Impl;
    explicit MppDecoderNode(rkvc::Request req);
    ~MppDecoderNode() override;
    std::string_view id() const noexcept override { return "mpp.decode"; }
    std::vector<rkvc::Port> make_ports() const override;
    rkvc::Status configure(std::vector<rkvc::Port>& ports,
                           rkvc::Diag* diag) override;
    rkvc::Status open(rkvc::Emit* emit, rkvc::Diag* diag) override;
    rkvc::Status process(rkvc::FramePtr input, rkvc::Diag* diag) override;
    rkvc::Status flush(rkvc::Diag* diag) override;
    void close() noexcept override;

private:
    std::unique_ptr<Impl> impl_;
};

class MppDecodeFactory : public rkvc::Factory {
public:
    std::string_view id() const noexcept override { return "mpp.decode"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Decode;
    }
    int priority() const noexcept override { return 1000; }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override;
    int score(const rkvc::Request& r,
              const rkvc::DeviceCaps&) const noexcept override;
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request& r,
                                       rkvc::Diag* diag) const override;
};

}  // namespace h264h265
