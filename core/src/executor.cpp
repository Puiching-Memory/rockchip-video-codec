// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/executor.hpp"

#include <utility>

namespace rkvc {

Executor::~Executor() { shutdown(); }

Status Executor::launch(Graph& g, FrameQueue* in_q, FrameQueue* out_q) {
    if (launched_ || !in_q || !out_q || g.node_count() < 2)
        return Status::Invalid;
    g_ = &g;
    in_q_ = in_q;
    out_q_ = out_q;
    launched_ = true;
    for (size_t i = 0; i < g_->node_count(); ++i)
        threads_.emplace_back([this, i] { worker(i); });
    return Status::Ok;
}

void Executor::shutdown() {
    if (!launched_)
        return;
    if (in_q_)
        in_q_->cancel(Status::Canceled);
    if (out_q_)
        out_q_->cancel(Status::Canceled);
    if (g_) {
        for (size_t i = 0; i < g_->edge_count(); ++i)
            g_->edge(i)->cancel(Status::Canceled);
    }
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();
    launched_ = false;
}

Status Executor::error() const noexcept {
    std::lock_guard<std::mutex> lk(m_);
    return error_;
}

Diag Executor::error_diag() const {
    std::lock_guard<std::mutex> lk(m_);
    return error_diag_;
}

void Executor::fail(Status s, Diag d) {
    if (s == Status::Ok || s == Status::Again || s == Status::Eof)
        return;
    {
        std::lock_guard<std::mutex> lk(m_);
        if (error_ != Status::Ok)
            return;
        error_ = s;
        error_diag_ = std::move(d);
    }
    // Two-phase termination: cancel inputs and edges, but NOT the session
    // output queue. The sink keeps draining buffered frames into it and
    // cancels it with the error once the stream breaks, so fully processed
    // frames are never lost to a racing cancel.
    if (in_q_)
        in_q_->cancel(s);
    if (g_) {
        for (size_t i = 0; i < g_->edge_count(); ++i)
            g_->edge(i)->cancel(s);
    }
}

void Executor::worker(size_t idx) {
    Graph& g = *g_;
    Node* n = g.node(idx);
    if (idx == 0) {
        for (;;) {
            Diag d;
            Status st = n->process(nullptr, &d);
            if (st == Status::Eof) {
                Diag fd;
                Status fs = n->flush(&fd);
                if (fs != Status::Ok) {
                    fail(fs, std::move(fd));
                    return;
                }
                if (g.edge_count() > 0)
                    g.edge(0)->close();
                return;
            }
            if (st != Status::Ok) {
                fail(st, std::move(d));
                return;
            }
        }
    }
    FrameQueue* in = g.edge(idx - 1);
    bool has_out = idx < g.edge_count();
    bool is_sink = !has_out;
    for (;;) {
        auto r = in->pop();
        if (!r) {
            if (r.status() != Status::Eof) {
                fail(r.status(), Diag{});
                // Terminal for the session side too, or pull() would hang.
                if (is_sink)
                    out_q_->cancel(r.status());
                return;
            }
            Diag fd;
            Status fs = n->flush(&fd);
            if (fs != Status::Ok) {
                fail(fs, std::move(fd));
                if (is_sink)
                    out_q_->cancel(fs);
                return;
            }
            if (has_out)
                g.edge(idx)->close();
            else
                out_q_->close();
            return;
        }
        Diag d;
        Status st = n->process(r.value(), &d);
        if (st != Status::Ok) {
            fail(st, std::move(d));
            if (is_sink)
                out_q_->cancel(st);
            return;
        }
    }
}

}  // namespace rkvc
