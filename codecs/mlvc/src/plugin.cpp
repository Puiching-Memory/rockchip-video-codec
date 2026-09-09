// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/codec.hpp"

#include "rkvc/plugin.hpp"

namespace {

const rkvc::Factory* g_factories_storage[2] = {nullptr, nullptr};

struct MlvcPlugin {
    mlvc::MlvcEncodeFactory enc;
    mlvc::MlvcDecodeFactory dec;
};

MlvcPlugin& plugin() {
    static MlvcPlugin p;
    return p;
}

rkvc::PluginDescriptor g_desc{rkvc::kPluginAbi, "mlvc", "0.5.0",
                              RKVC_TOOLCHAIN_FINGERPRINT, nullptr, 0};

}  // namespace

extern "C" const rkvc::PluginDescriptor* rkvc_plugin_query(
    uint32_t host_abi) noexcept {
    if (host_abi != rkvc::kPluginAbi)
        return nullptr;
    g_factories_storage[0] = &plugin().enc;
    g_factories_storage[1] = &plugin().dec;
    g_desc.factories = g_factories_storage;
    g_desc.factory_count = 2;
    return &g_desc;
}
