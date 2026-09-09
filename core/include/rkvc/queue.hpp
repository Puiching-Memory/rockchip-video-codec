// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>

#include "rkvc/result.hpp"
#include "rkvc/status.hpp"

namespace rkvc {

// Bounded FIFO with tri-state pop: item / Eof (closed+drained) / error.
// Buffered items are always delivered before the terminal state.
template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(size_t capacity)
        : capacity_(capacity ? capacity : 1) {}

    Status push(T v) {
        std::unique_lock<std::mutex> lk(m_);
        not_full_.wait(lk, [&] { return queue_.size() < capacity_ || end_; });
        if (end_)
            return end_status_;
        queue_.push(std::move(v));
        not_empty_.notify_one();
        return Status::Ok;
    }

    Status try_push(T v) {
        std::lock_guard<std::mutex> lk(m_);
        if (end_)
            return end_status_;
        if (queue_.size() >= capacity_)
            return Status::Again;
        queue_.push(std::move(v));
        not_empty_.notify_one();
        return Status::Ok;
    }

    Result<T> pop() {
        std::unique_lock<std::mutex> lk(m_);
        not_empty_.wait(lk, [&] { return !queue_.empty() || end_; });
        if (!queue_.empty()) {
            T v = std::move(queue_.front());
            queue_.pop();
            not_full_.notify_one();
            return Result<T>::success(std::move(v));
        }
        return Result<T>::failure(end_status_);
    }

    Result<T> try_pop() {
        std::lock_guard<std::mutex> lk(m_);
        if (!queue_.empty()) {
            T v = std::move(queue_.front());
            queue_.pop();
            not_full_.notify_one();
            return Result<T>::success(std::move(v));
        }
        if (end_)
            return Result<T>::failure(end_status_);
        return Result<T>::failure(Status::Again);
    }

    void close() {
        std::lock_guard<std::mutex> lk(m_);
        if (!end_) {
            end_ = true;
            end_status_ = Status::Eof;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    void cancel(Status s) {
        if (s == Status::Ok || s == Status::Again || s == Status::Eof)
            return;
        std::lock_guard<std::mutex> lk(m_);
        if (!end_) {
            end_ = true;
            end_status_ = s;
        }
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(m_);
        return queue_.size();
    }

private:
    size_t capacity_;
    mutable std::mutex m_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::queue<T> queue_;
    bool end_ = false;
    Status end_status_ = Status::Eof;
};

}  // namespace rkvc
