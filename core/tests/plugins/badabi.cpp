// SPDX-License-Identifier: AGPL-3.0-or-later
// Bad-ABI plugin: the loader must evict it without touching the registry.
#include <cstddef>

#include "rkvc/plugin.hpp"

namespace {

rkvc::PluginDescriptor g_desc{0xFFFFFFFFu, "test-badabi", "0.5.0",
                              RKVC_TOOLCHAIN_FINGERPRINT, nullptr, 0};

}  // namespace

extern "C" const rkvc::PluginDescriptor* rkvc_plugin_query(
    uint32_t) noexcept {
    return &g_desc;
}
