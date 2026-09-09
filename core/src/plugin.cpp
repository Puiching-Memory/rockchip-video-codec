// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/plugin.hpp"

#include <dlfcn.h>

#include "rkvc/context.hpp"

namespace rkvc {

Context::~Context() {
    for (auto& p : plugins_)
        if (p.handle)
            dlclose(p.handle);
}

Status Context::load_plugin(const std::string& path, Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("load", path.empty() ? "<empty>" : path.c_str(),
                      reason);
        return s;
    };
    if (path.empty())
        return reject(Status::Invalid, "empty path");
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        if (diag) {
            const char* err = dlerror();
            diag->add("load", path.c_str(),
                      err && *err ? err : "dlopen failed");
        }
        return Status::NotFound;
    }
    auto query =
        reinterpret_cast<PluginQueryFn>(dlsym(handle, RKVC_PLUGIN_QUERY_SYMBOL));
    if (!query) {
        dlclose(handle);
        return reject(Status::NotFound, "missing query symbol");
    }
    const PluginDescriptor* desc = query(kPluginAbi);
    if (!desc) {
        dlclose(handle);
        return reject(Status::Invalid, "null descriptor");
    }
    if (desc->abi != kPluginAbi) {
        dlclose(handle);
        return reject(Status::Unsupported, "abi mismatch");
    }
    if (!desc->name || !*desc->name || !desc->version || !*desc->version ||
        !desc->toolchain || !*desc->toolchain) {
        dlclose(handle);
        return reject(Status::Invalid, "bad descriptor strings");
    }
    if (std::string(desc->toolchain) != RKVC_TOOLCHAIN_FINGERPRINT) {
        dlclose(handle);
        return reject(Status::Unsupported, "toolchain mismatch");
    }
    if (desc->factory_count > 0 && !desc->factories) {
        dlclose(handle);
        return reject(Status::Invalid, "null factory table");
    }
    for (size_t i = 0; i < desc->factory_count; ++i) {
        const Factory* f = desc->factories[i];
        if (!f || f->id().empty() || registry_.find(f->id()) != nullptr) {
            dlclose(handle);
            return reject(Status::Invalid, "bad or duplicate factory");
        }
    }
    for (size_t i = 0; i < desc->factory_count; ++i)
        registry_.add_borrowed(desc->factories[i]);
    LoadedPlugin lp;
    lp.handle = handle;
    lp.path = path;
    plugins_.push_back(std::move(lp));
    return Status::Ok;
}

}  // namespace rkvc
