// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <memory>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/node.hpp"
#include "rkvc/queue.hpp"
#include "rkvc/registry.hpp"
#include "rkvc/request.hpp"
#include "rkvc/result.hpp"

namespace rkvc {

class Context;

// Linear plan: adapter or candidate-factory steps with backtracking fallback.
struct PlanStep {
    NodeStage stage = NodeStage::Transform;
    std::vector<const Factory*> candidates;
    size_t index = 0;
    bool adapter_source = false;
    bool adapter_sink = false;
};

struct Plan {
    static constexpr size_t kMaxFallbacks = 8;
    std::vector<PlanStep> steps;
    size_t fallbacks = 0;
    // Advance the failed step to its next candidate. False = exhausted.
    bool advance(size_t failed_step) noexcept;
};

using FrameQueue = BoundedQueue<FramePtr>;

class Graph {
public:
    static Result<std::unique_ptr<Graph>> build(const Plan& plan, Context& ctx,
                                                const Request& req,
                                                FrameQueue* in_q,
                                                FrameQueue* out_q,
                                                size_t* failed_step,
                                                Diag* diag = nullptr);
    Status open(Diag* diag = nullptr);
    void close_nodes() noexcept;

    size_t failure_step() const noexcept { return failure_step_; }
    size_t node_count() const noexcept { return nodes_.size(); }
    Node* node(size_t i) const noexcept { return nodes_[i].get(); }
    std::vector<Port>& ports(size_t i) noexcept { return ports_[i]; }
    // Edge i connects node i to node i+1; edge_count = node_count - 1.
    FrameQueue* edge(size_t i) const noexcept { return edges_[i].get(); }
    size_t edge_count() const noexcept { return edges_.size(); }
    Emit* emit(size_t i) const noexcept { return emits_[i].get(); }

private:
    Graph() = default;
    std::vector<NodePtr> nodes_;
    std::vector<std::vector<Port>> ports_;
    std::vector<std::unique_ptr<FrameQueue>> edges_;
    std::vector<std::unique_ptr<Emit>> emits_;
    size_t failure_step_ = 0;
};

// FRAME_SINK endpoint adapters built directly into the plan.
NodePtr make_queue_source(FrameQueue* in_q, const Spec& spec);
NodePtr make_queue_sink(FrameQueue* out_q, const Spec& spec);

}  // namespace rkvc
