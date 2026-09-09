// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/node.hpp"

namespace rkvc {

Status Node::configure(std::vector<Port>& ports, Diag* diag) {
    for (const auto& p : ports) {
        if (p.resolved.fmt == PixelFormat::Unknown) {
            if (diag)
                diag->add("configure", std::string(id()), "port unresolved");
            return Status::Negotiate;
        }
    }
    return Status::Ok;
}

}  // namespace rkvc
