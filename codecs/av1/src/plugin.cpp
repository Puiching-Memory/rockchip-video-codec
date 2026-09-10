// SPDX-License-Identifier: AGPL-3.0-or-later
#include "av1/encoder.hpp"

#include "rkvc/plugin.hpp"

namespace {

av1::SvtEncodeFactory g_factory;
const rkvc::Factory* g_factories[] = {&g_factory};
rkvc::PluginDescriptor g_desc{rkvc::kPluginAbi,           "av1",       "0.5.0",
                              RKVC_TOOLCHAIN_FINGERPRINT, g_factories, 1};

}  // namespace

extern "C" const rkvc::PluginDescriptor* rkvc_plugin_query(
    uint32_t host_abi) noexcept {
    if (host_abi != rkvc::kPluginAbi)
        return nullptr;
    return &g_desc;
}
