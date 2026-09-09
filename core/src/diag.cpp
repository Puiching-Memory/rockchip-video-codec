// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/diag.hpp"

namespace rkvc {

void Diag::add(std::string stage, std::string subject, std::string reason) {
    entries_.push_back(
        DiagEntry{std::move(stage), std::move(subject), std::move(reason)});
}

std::string Diag::format() const {
    std::string out;
    for (const auto& e : entries_) {
        out += e.stage;
        out += "(";
        out += e.subject;
        out += "): ";
        out += e.reason;
        out += "\n";
    }
    return out;
}

}  // namespace rkvc
