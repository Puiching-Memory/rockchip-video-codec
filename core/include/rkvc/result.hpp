// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <new>
#include <type_traits>
#include <utility>

#include "rkvc/diag.hpp"
#include "rkvc/status.hpp"

namespace rkvc {

template <typename T>
class Result {
public:
    static Result success(T value) {
        Result r;
        r.ok_ = true;
        new (&r.storage_) T(std::move(value));
        return r;
    }
    static Result failure(Status s, Diag d = {}) {
        Result r;
        r.ok_ = false;
        r.status_ = s;
        r.diag_ = std::move(d);
        return r;
    }

    Result(Result&& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        ok_ = o.ok_;
        if (ok_) {
            new (&storage_) T(std::move(o.value()));
        } else {
            status_ = o.status_;
            diag_ = std::move(o.diag_);
        }
    }
    Result& operator=(Result&& o) noexcept(std::is_nothrow_move_assignable_v<T>) {
        if (this != &o) {
            reset();
            ok_ = o.ok_;
            if (ok_) {
                new (&storage_) T(std::move(o.value()));
            } else {
                status_ = o.status_;
                diag_ = std::move(o.diag_);
            }
        }
        return *this;
    }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    ~Result() { reset(); }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    Status status() const noexcept { return ok_ ? Status::Ok : status_; }
    const Diag& diag() const noexcept { return diag_; }
    T& value() & { return *reinterpret_cast<T*>(&storage_); }
    const T& value() const& { return *reinterpret_cast<const T*>(&storage_); }
    T&& value() && { return std::move(*reinterpret_cast<T*>(&storage_)); }

private:
    Result() : ok_(false), status_(Status::Internal) {}
    void reset() noexcept {
        if (ok_) {
            reinterpret_cast<T*>(&storage_)->~T();
            ok_ = false;
        }
    }

    bool ok_;
    Status status_{Status::Internal};
    Diag diag_;
    typename std::aligned_storage<sizeof(T), alignof(T)>::type storage_;
};

template <>
class Result<void> {
public:
    static Result success() {
        Result r;
        r.ok_ = true;
        return r;
    }
    static Result failure(Status s, Diag d = {}) {
        Result r;
        r.ok_ = false;
        r.status_ = s;
        r.diag_ = std::move(d);
        return r;
    }

    bool ok() const noexcept { return ok_; }
    explicit operator bool() const noexcept { return ok_; }
    Status status() const noexcept { return ok_ ? Status::Ok : status_; }
    const Diag& diag() const noexcept { return diag_; }

private:
    Result() : ok_(false), status_(Status::Internal) {}
    bool ok_;
    Status status_{Status::Internal};
    Diag diag_;
};

}  // namespace rkvc
