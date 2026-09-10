// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>
#include <string_view>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/node.hpp"
#include "rkvc/request.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

enum class NodeStage : uint32_t {
    Source = 0,
    Decode,
    Transform,
    Encode,
    Sink,
};

enum class Transport : uint32_t {
    Internal = 0,  // in-process stage, no endpoint semantic
    Queue,         // FrameSink endpoint adapter
    File,          // file endpoint node (core-external, Phase 2)
};

struct Factory {
    virtual ~Factory() = default;
    virtual std::string_view id() const noexcept = 0;
    virtual NodeStage stage() const noexcept = 0;
    virtual Transport transport() const noexcept { return Transport::Internal; }
    virtual int priority() const noexcept { return 0; }
    virtual bool matches(const Request& r,
                         const DeviceCaps& caps) const noexcept = 0;
    virtual int score(const Request& r,
                      const DeviceCaps& caps) const noexcept {
        (void)r;
        (void)caps;
        return 0;
    }
    virtual Result<NodePtr> create(const Request& r, Diag* diag) const = 0;
};

class Registry {
public:
    Status add(std::unique_ptr<Factory> f);
    /// Borrowed factories stay owned by the caller (e.g. a loaded plugin);
    /// the caller must outlive the registry.
    Status add_borrowed(const Factory* f);
    /// Candidates sorted by (priority+score desc, id asc). Never null entries.
    std::vector<const Factory*> candidates(NodeStage stage, const Request& r,
                                           const DeviceCaps& caps) const;
    const Factory* find(std::string_view id) const noexcept;
    size_t size() const noexcept { return factories_.size(); }

private:
    std::vector<std::unique_ptr<Factory>> factories_;
    std::vector<const Factory*> borrowed_;
};

}  // namespace rkvc
