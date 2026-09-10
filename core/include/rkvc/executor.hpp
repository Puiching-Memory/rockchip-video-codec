// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/graph.hpp"
#include "rkvc/queue.hpp"
#include "rkvc/status.hpp"

namespace rkvc {

/// Owns per-node worker threads: source ticks, middles pop/process/push,
/// sink drains to the session output queue. First error cancels everything.
class Executor {
public:
    Executor() = default;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;
    ~Executor();

    Status launch(Graph& g, FrameQueue* in_q, FrameQueue* out_q);
    void shutdown();
    /// Blocks until all workers exit; returns the first error or Ok.
    Status wait();
    Status error() const noexcept;
    Diag error_diag() const;

private:
    void fail(Status s, Diag d);
    void worker(size_t idx);

    Graph* g_ = nullptr;
    FrameQueue* in_q_ = nullptr;
    FrameQueue* out_q_ = nullptr;
    std::vector<std::thread> threads_;
    mutable std::mutex m_;
    std::condition_variable done_;
    size_t active_ = 0;
    Status error_ = Status::Ok;
    Diag error_diag_;
    bool launched_ = false;
};

}  // namespace rkvc
