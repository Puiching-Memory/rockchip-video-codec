// SPDX-License-Identifier: AGPL-3.0-or-later
// Identity transform test plugin: exercises dlopen + handshake + borrowed
// factory registration + a real session through a plugin node.
#include <cstddef>
#include <new>

#include "rkvc/node.hpp"
#include "rkvc/plugin.hpp"

namespace {

class IdentityNode : public rkvc::Node {
public:
    std::string_view id() const noexcept override { return "test.identity"; }
    std::vector<rkvc::Port> make_ports() const override {
        rkvc::Port in, out;
        in.name = "in";
        in.is_input = true;
        out.name = "out";
        return {in, out};
    }
    rkvc::Status open(rkvc::Emit* emit, rkvc::Diag* diag) override {
        if (!emit) {
            if (diag)
                diag->add("open", "test.identity", "null emit");
            return rkvc::Status::Invalid;
        }
        emit_ = emit;
        return rkvc::Status::Ok;
    }
    rkvc::Status process(rkvc::FramePtr input, rkvc::Diag* diag) override {
        if (!input) {
            if (diag)
                diag->add("process", "test.identity", "null frame");
            return rkvc::Status::Invalid;
        }
        return emit_->emit(0, std::move(input));
    }

private:
    rkvc::Emit* emit_ = nullptr;
};

class IdentityFactory : public rkvc::Factory {
public:
    std::string_view id() const noexcept override { return "test.identity"; }
    rkvc::NodeStage stage() const noexcept override {
        return rkvc::NodeStage::Transform;
    }
    bool matches(const rkvc::Request& r,
                 const rkvc::DeviceCaps&) const noexcept override {
        return r.operation == rkvc::Operation::Upscale;
    }
    rkvc::Result<rkvc::NodePtr> create(const rkvc::Request&,
                                       rkvc::Diag*) const override {
        rkvc::NodePtr n(new (std::nothrow) IdentityNode());
        if (!n)
            return rkvc::Result<rkvc::NodePtr>::failure(rkvc::Status::Nomem);
        return rkvc::Result<rkvc::NodePtr>::success(std::move(n));
    }
};

IdentityFactory g_factory;
const rkvc::Factory* g_factories[] = {&g_factory};
rkvc::PluginDescriptor g_desc{rkvc::kPluginAbi, "test-identity",
                              "0.5.0",          RKVC_TOOLCHAIN_FINGERPRINT,
                              g_factories,      1};

}  // namespace

extern "C" const rkvc::PluginDescriptor* rkvc_plugin_query(
    uint32_t host_abi) noexcept {
    if (host_abi != rkvc::kPluginAbi)
        return nullptr;
    return &g_desc;
}
