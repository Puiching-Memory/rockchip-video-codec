// SPDX-License-Identifier: AGPL-3.0-or-later
#include "h264h265/decoder.hpp"
#include "h264h265/encoder.hpp"

#include "rkvc/plugin.hpp"

namespace {

h264h265::MppDecodeFactory g_decode_factory;
h264h265::MppEncodeFactory g_encode_factory;
const rkvc::Factory* g_factories[] = {&g_decode_factory, &g_encode_factory};
rkvc::PluginDescriptor g_desc{rkvc::kPluginAbi, "h264h265", "0.5.0",
                              RKVC_TOOLCHAIN_FINGERPRINT, g_factories, 2};

}  // namespace

extern "C" const rkvc::PluginDescriptor* rkvc_plugin_query(
    uint32_t host_abi) noexcept {
    if (host_abi != rkvc::kPluginAbi)
        return nullptr;
    return &g_desc;
}
