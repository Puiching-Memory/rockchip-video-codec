// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "rkvc/registry.hpp"

namespace rkvc {

// Codec plugin ABI. Rules for both sides (same toolchain, same release):
// - every virtual returns Status (or a trivial value); no exception crosses
//   the boundary (core builds -fno-exceptions -fno-rtti; plugins must too);
// - interface classes carry no data members; factories and nodes stay owned
//   by the plugin, nodes created per session are owned by the graph;
// - ownership never crosses: strings are views, frames are shared_ptr on a
//   shared heap, descriptors are plugin-static storage.
constexpr uint32_t kPluginAbi = 1;
#define RKVC_PLUGIN_QUERY_SYMBOL "rkvc_plugin_query"

// g++ version + language/ABI switches that must match to share C++ types.
#define RKVC_TOOLCHAIN_FINGERPRINT \
    ("g++-" __VERSION__ "-c++20-noexc-nortti")

struct HostServices {
    uint32_t abi = 0;  // kPluginAbi, filled by the host
    void (*log)(void* ctx, int level, const char* msg) noexcept = nullptr;
    void* log_ctx = nullptr;
};

struct PluginDescriptor {
    uint32_t abi = 0;  // must equal kPluginAbi
    const char* name = nullptr;
    const char* version = nullptr;
    const char* toolchain = nullptr;  // RKVC_TOOLCHAIN_FINGERPRINT
    const Factory* const* factories = nullptr;
    size_t factory_count = 0;
};

using PluginQueryFn = const PluginDescriptor* (*)(uint32_t host_abi) noexcept;

struct LoadedPlugin {
    void* handle = nullptr;
    std::string path;
};

}  // namespace rkvc
