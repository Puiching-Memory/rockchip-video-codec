// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/registry.hpp"

#include <algorithm>

namespace rkvc {

Status Registry::add(std::unique_ptr<Factory> f) {
    if (!f)
        return Status::Invalid;
    if (find(f->id()) != nullptr)
        return Status::Invalid;
    factories_.push_back(std::move(f));
    return Status::Ok;
}

std::vector<const Factory*> Registry::candidates(NodeStage stage,
                                                 const Request& r,
                                                 const DeviceCaps& caps) const {
    std::vector<const Factory*> out;
    for (const auto& f : factories_) {
        if (f->stage() != stage)
            continue;
        if (!f->matches(r, caps))
            continue;
        out.push_back(f.get());
    }
    std::sort(out.begin(), out.end(), [&](const Factory* a, const Factory* b) {
        int sa = a->priority() + a->score(r, caps);
        int sb = b->priority() + b->score(r, caps);
        if (sa != sb)
            return sa > sb;
        return a->id() < b->id();
    });
    return out;
}

const Factory* Registry::find(std::string_view id) const noexcept {
    for (const auto& f : factories_)
        if (f->id() == id)
            return f.get();
    return nullptr;
}

}  // namespace rkvc
