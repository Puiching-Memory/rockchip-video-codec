// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>

#include "rkvc/node.hpp"
#include "rkvc/registry.hpp"

namespace av1 {

// SVT-AV1 software encoder node ("svt.encode", ENCODE stage).
class SvtEncoderNode : public rkvc::Node {
public:
    struct Impl;
    explicit SvtEncoderNode(rkvc::Request req);
    ~SvtEncoderNode() override;
    std::string_view id() const noexcept override { return "svt.encode"; }
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

class SvtEncodeFactory : public rkvc::Factory {
public:
    std::string_view id() const noexcept override { return "svt.encode"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Encode;
    }
    int priority() const noexcept override { return 500; }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override;
    int score(const rkvc::Request& r,
              const rkvc::DeviceCaps&) const noexcept override;
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request& r,
                                       rkvc::Diag* diag) const override;
};

}  // namespace av1
