// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/session.hpp"

#include <new>
#include <utility>

#include "rkvc/context.hpp"

namespace rkvc {

Result<std::shared_ptr<Session>> Session::create(Context& ctx,
                                                 const Request& req,
                                                 Diag* diag) {
    auto reject = [&](Status s, const char* reason) {
        if (diag)
            diag->add("create", "session", reason);
        return Result<std::shared_ptr<Session>>::failure(
            s, diag ? *diag : Diag{});
    };
    if (!validate(req, diag))
        return reject(Status::Invalid, "bad request");
    std::shared_ptr<Session> s(new (std::nothrow) Session());
    if (!s)
        return reject(Status::Nomem, "session alloc failed");
    s->ctx_ = &ctx;
    s->req_ = req;
    s->in_.reset(new (std::nothrow) FrameQueue(req.queue_capacity));
    s->out_.reset(new (std::nothrow) FrameQueue(req.queue_capacity));
    if (!s->in_ || !s->out_)
        return reject(Status::Nomem, "queue alloc failed");
    auto pr = ctx.plan(req, diag);
    if (!pr)
        return Result<std::shared_ptr<Session>>::failure(pr.status(),
                                                         pr.diag());
    s->plan_ = std::move(pr.value());
    for (;;) {
        size_t failed = 0;
        auto gr = Graph::build(s->plan_, ctx, req, s->in_.get(), s->out_.get(),
                               &failed, diag);
        if (gr) {
            s->graph_ = std::move(gr.value());
            return Result<std::shared_ptr<Session>>::success(std::move(s));
        }
        if ((gr.status() != Status::Negotiate &&
             gr.status() != Status::Internal) ||
            !s->plan_.advance(failed)) {
            if (diag)
                diag->add("planner", "candidate",
                          "no further fallback candidate");
            return Result<std::shared_ptr<Session>>::failure(gr.status(),
                                                             gr.diag());
        }
        if (diag)
            diag->add("planner", "candidate",
                      "candidate rejected; trying fallback");
    }
}

Status Session::start(Diag* diag) {
    if (closed_)
        return Status::Invalid;
    if (started_)
        return Status::Ok;
    for (;;) {
        Status st = graph_->open(diag);
        if (st == Status::Ok)
            break;
        if (st != Status::Hw || !plan_.advance(graph_->failure_step()))
            return st;
        if (diag)
            diag->add("planner", "candidate",
                      "open failed; trying fallback candidate");
        for (;;) {
            size_t failed = 0;
            auto gr = Graph::build(plan_, *ctx_, req_, in_.get(), out_.get(),
                                   &failed, diag);
            if (gr) {
                graph_ = std::move(gr.value());
                break;
            }
            if ((gr.status() != Status::Negotiate &&
                 gr.status() != Status::Internal) ||
                !plan_.advance(failed))
                return gr.status();
            if (diag)
                diag->add("planner", "candidate",
                          "candidate rejected; trying fallback");
        }
    }
    Status st = exec_.launch(*graph_, in_.get(), out_.get());
    if (st != Status::Ok)
        return st;
    started_ = true;
    return Status::Ok;
}

Status Session::push(FramePtr f) {
    if (closed_)
        return Status::Canceled;
    if (!f)
        return Status::Invalid;
    return in_->try_push(std::move(f));
}

Status Session::push_eos() {
    if (closed_)
        return Status::Canceled;
    in_->close();
    return Status::Ok;
}

Result<FramePtr> Session::try_pull() { return out_->try_pop(); }

Result<FramePtr> Session::pull() { return out_->pop(); }

Status Session::wait() { return exec_.wait(); }

Diag Session::error_diag() const { return exec_.error_diag(); }

Status Session::close() {
    if (closed_)
        return Status::Ok;
    closed_ = true;
    in_->close();
    exec_.shutdown();
    if (graph_)
        graph_->close_nodes();
    Status err = exec_.error();
    return err;
}

Session::~Session() { close(); }

}  // namespace rkvc
