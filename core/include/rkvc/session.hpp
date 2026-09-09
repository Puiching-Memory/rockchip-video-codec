// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <memory>

#include "rkvc/diag.hpp"
#include "rkvc/executor.hpp"
#include "rkvc/graph.hpp"
#include "rkvc/node.hpp"
#include "rkvc/request.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

class Context;

// Streaming session: push (non-blocking) / pull (blocking) / try_pull
// (non-blocking) / push_eos. Triple-state contract: Ok / Eof / Again.
class Session {
public:
    static Result<std::shared_ptr<Session>> create(Context& ctx,
                                                   const Request& req,
                                                   Diag* diag = nullptr);
    Status start(Diag* diag = nullptr);
    Status push(FramePtr f);
    Status push_eos();
    Result<FramePtr> try_pull();
    Result<FramePtr> pull();
    Status close();
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

private:
    Session() = default;
    Context* ctx_ = nullptr;
    Request req_;
    Plan plan_;
    std::unique_ptr<Graph> graph_;
    Executor exec_;
    std::unique_ptr<FrameQueue> in_;
    std::unique_ptr<FrameQueue> out_;
    bool started_ = false;
    bool closed_ = false;
};

}  // namespace rkvc
