// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/request.hpp"

namespace rkvc {

Result<void> validate(const Request& r, Diag* diag) {
    auto reject = [&](const char* reason) {
        if (diag)
            diag->add("validate", "request", reason);
        return Result<void>::failure(Status::Invalid,
                                     diag ? *diag : Diag{});
    };
    if (r.queue_capacity == 0)
        return reject("queue capacity must be nonzero");
    if (r.input.kind == EndpointKind::File && r.input.uri.empty())
        return reject("file input needs uri");
    if (r.output.kind == EndpointKind::File && r.output.uri.empty())
        return reject("file output needs uri");
    if (r.quality.qp < -1 || r.quality.qp > 63)
        return reject("qp out of range");
    if (r.quality.bitrate_bps < 0)
        return reject("negative bitrate");
    return Result<void>::success();
}

}  // namespace rkvc
