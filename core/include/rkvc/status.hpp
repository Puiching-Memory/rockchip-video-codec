// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

namespace rkvc {

enum class Status : int {
    Ok = 0,
    Nomem = -1,
    Invalid = -2,
    NotFound = -3,
    Io = -4,
    Hw = -5,
    Eof = -6,
    Again = -7,
    Format = -8,
    Negotiate = -9,
    Permission = -10,
    Canceled = -11,
    Unsupported = -12,
    Internal = -13,
    Model = -14,
    License = -15,
    Integrity = -16,
};

inline bool is_ok(Status s) {
    return s == Status::Ok;
}
inline bool is_flow(Status s) {
    return s == Status::Again || s == Status::Eof;
}

const char* to_string(Status s) noexcept;

}  // namespace rkvc
