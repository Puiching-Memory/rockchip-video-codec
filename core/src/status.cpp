// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/status.hpp"

namespace rkvc {

const char* to_string(Status s) noexcept {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::Nomem: return "no memory";
        case Status::Invalid: return "invalid argument";
        case Status::NotFound: return "not found";
        case Status::Io: return "i/o error";
        case Status::Hw: return "hardware error";
        case Status::Eof: return "end of stream";
        case Status::Again: return "try again";
        case Status::Format: return "format mismatch";
        case Status::Negotiate: return "negotiation failed";
        case Status::Permission: return "permission denied";
        case Status::Canceled: return "canceled";
        case Status::Unsupported: return "unsupported";
        case Status::Internal: return "internal error";
        case Status::Model: return "model error";
        case Status::License: return "license error";
        case Status::Integrity: return "integrity error";
    }
    return "unknown";
}

}  // namespace rkvc
