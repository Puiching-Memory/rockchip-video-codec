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

Status Registry::add_borrowed(const Factory* f) {
    if (!f || f->id().empty())
        return Status::Invalid;
    if (find(f->id()) != nullptr)
        return Status::Invalid;
    borrowed_.push_back(f);
    return Status::Ok;
}

std::vector<const Factory*> Registry::candidates(NodeStage stage,
                                                 const Request& r,
                                                 const DeviceCaps& caps) const {
    std::vector<const Factory*> out;
    auto consider = [&](const Factory* f) {
        if (f->stage() != stage)
            return;
        if (!f->matches(r, caps))
            return;
        out.push_back(f);
    };
    for (const auto& f : factories_)
        consider(f.get());
    for (const Factory* f : borrowed_)
        consider(f);
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
    for (const Factory* f : borrowed_)
        if (f && f->id() == id)
            return f;
    return nullptr;
}

}  // namespace rkvc
