// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace rkvc {

struct DiagEntry {
    std::string stage;
    std::string subject;
    std::string reason;
};

class Diag {
public:
    Diag() = default;
    void add(std::string stage, std::string subject, std::string reason);
    bool empty() const noexcept { return entries_.empty(); }
    size_t size() const noexcept { return entries_.size(); }
    const DiagEntry& at(size_t i) const { return entries_.at(i); }
    std::string format() const;

private:
    std::vector<DiagEntry> entries_;
};

}  // namespace rkvc
