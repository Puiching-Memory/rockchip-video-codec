// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/graph.hpp"
#include "rkvc/node.hpp"
#include "rkvc/plugin.hpp"
#include "rkvc/registry.hpp"
#include "rkvc/request.hpp"
#include "rkvc/result.hpp"
#include "rkvc/rkmodel.hpp"

namespace rkvc {

struct ContextOptions {
    std::vector<std::string> backend_dirs;
};

/// Owns the factory registry, model store, and device probe cache.
class Context {
public:
    explicit Context(ContextOptions opts = {});
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    Registry& registry() noexcept { return registry_; }
    const Registry& registry() const noexcept { return registry_; }

    Status load_plugin(const std::string& path, Diag* diag = nullptr);
    size_t plugin_count() const noexcept { return plugins_.size(); }

    Status add_model(Model m);
    const Model* find_model(const std::string& id) const noexcept;
    const Model* first_model() const noexcept;

    /// Declared before registry_: borrowed plugin factories must drop before
    /// dlclose, and members destroy in reverse declaration order.
    std::vector<LoadedPlugin> plugins_;
    DeviceCaps probe_device();
    void override_device_caps(const DeviceCaps& caps);

    Result<Plan> plan(const Request& req, Diag* diag = nullptr);

private:
    ContextOptions opts_;
    Registry registry_;
    std::vector<Model> models_;
    DeviceCaps caps_;
    bool caps_probed_ = false;
};

}  // namespace rkvc
