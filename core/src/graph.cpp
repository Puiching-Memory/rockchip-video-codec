// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/graph.hpp"

#include <new>
#include <utility>

#include "rkvc/context.hpp"

namespace rkvc {

namespace {

// Linear edge delivery into the next node's input queue.
class NodeEmit : public Emit {
public:
    explicit NodeEmit(std::vector<FrameQueue*> outs) : outs_(std::move(outs)) {}
    Status emit(size_t port, FramePtr f) override {
        if (!f)
            return Status::Invalid;
        if (port >= outs_.size() || !outs_[port])
            return Status::Invalid;
        return outs_[port]->push(std::move(f));
    }

private:
    std::vector<FrameQueue*> outs_;
};

// FRAME_SINK endpoint adapters (first/last pipeline nodes).
class QueueSourceNode : public Node {
public:
    QueueSourceNode(FrameQueue* q, Spec spec) : q_(q), spec_(spec) {}
    std::string_view id() const noexcept override { return "queue.source"; }
    std::vector<Port> make_ports() const override {
        Port p;
        p.name = "out";
        p.is_input = false;
        p.desired = spec_;
        return {p};
    }
    Status open(Emit* emit, Diag* diag) override {
        if (!emit || !q_) {
            if (diag)
                diag->add("open", "queue.source", "null emit or queue");
            return Status::Invalid;
        }
        emit_ = emit;
        return Status::Ok;
    }
    Status process(FramePtr /*input*/, Diag* /*diag*/) override {
        auto r = q_->pop();
        if (!r)
            return r.status();
        return emit_->emit(0, r.value());
    }

private:
    FrameQueue* q_;
    Spec spec_;
    Emit* emit_ = nullptr;
};

class QueueSinkNode : public Node {
public:
    QueueSinkNode(FrameQueue* q, Spec spec) : q_(q), spec_(spec) {}
    std::string_view id() const noexcept override { return "queue.sink"; }
    std::vector<Port> make_ports() const override {
        Port p;
        p.name = "in";
        p.is_input = true;
        p.desired = spec_;
        return {p};
    }
    Status open(Emit* /*emit*/, Diag* diag) override {
        if (!q_) {
            if (diag)
                diag->add("open", "queue.sink", "null queue");
            return Status::Invalid;
        }
        return Status::Ok;
    }
    Status process(FramePtr input, Diag* diag) override {
        if (!input) {
            if (diag)
                diag->add("process", "queue.sink", "null frame");
            return Status::Invalid;
        }
        return q_->push(std::move(input));
    }

private:
    FrameQueue* q_;
    Spec spec_;
};

}  // namespace

NodePtr make_queue_source(FrameQueue* in_q, const Spec& spec) {
    return NodePtr(new (std::nothrow) QueueSourceNode(in_q, spec));
}

NodePtr make_queue_sink(FrameQueue* out_q, const Spec& spec) {
    return NodePtr(new (std::nothrow) QueueSinkNode(out_q, spec));
}

bool Plan::advance(size_t failed_step) noexcept {
    if (fallbacks >= kMaxFallbacks || failed_step >= steps.size())
        return false;
    PlanStep& st = steps[failed_step];
    if (st.adapter_source || st.adapter_sink)
        return false;
    if (st.index + 1 >= st.candidates.size())
        return false;
    ++st.index;
    ++fallbacks;
    return true;
}

Result<std::unique_ptr<Graph>> Graph::build(const Plan& plan, Context& ctx,
                                            const Request& req,
                                            FrameQueue* in_q,
                                            FrameQueue* out_q,
                                            size_t* failed_step, Diag* diag) {
    auto fail_step = [&](size_t i, Status s, const char* reason) {
        if (failed_step)
            *failed_step = i;
        if (diag)
            diag->add("build", "graph", reason);
        return Result<std::unique_ptr<Graph>>::failure(
            s, diag ? *diag : Diag{});
    };
    auto fail_chain = [&](size_t i, Status s, const Diag& chain) {
        if (failed_step)
            *failed_step = i;
        return Result<std::unique_ptr<Graph>>::failure(s, chain);
    };
    if (!in_q || !out_q)
        return fail_step(0, Status::Invalid, "null endpoint queue");
    size_t n = plan.steps.size();
    if (n < 2)
        return fail_step(0, Status::Internal, "plan too short");

    std::unique_ptr<Graph> g(new (std::nothrow) Graph());
    if (!g)
        return fail_step(0, Status::Nomem, "graph alloc failed");

    // Caller intent merged with required dimensions (FRAME_SINK injection).
    Spec src_spec = req.input_spec;
    if (req.width) {
        if (src_spec.width && src_spec.width != req.width)
            return fail_step(0, Status::Negotiate, "width mismatch");
        src_spec.width = req.width;
    }
    if (req.height) {
        if (src_spec.height && src_spec.height != req.height)
            return fail_step(0, Status::Negotiate, "height mismatch");
        src_spec.height = req.height;
    }

    for (size_t i = 0; i < n; ++i) {
        const PlanStep& st = plan.steps[i];
        NodePtr node;
        if (st.adapter_source) {
            node = make_queue_source(in_q, src_spec);
            if (!node)
                return fail_step(i, Status::Nomem, "source alloc failed");
        } else if (st.adapter_sink) {
            node = make_queue_sink(out_q, req.output_spec);
            if (!node)
                return fail_step(i, Status::Nomem, "sink alloc failed");
        } else {
            if (st.index >= st.candidates.size() ||
                !st.candidates[st.index])
                return fail_step(i, Status::NotFound,
                                 "required stage has no candidate");
            auto r = st.candidates[st.index]->create(req, diag);
            if (!r)
                return fail_chain(i, r.status(), r.diag());
            node = std::move(r.value());
            if (!node)
                return fail_step(i, Status::Internal, "null node");
        }
        std::vector<Port> ports = node->make_ports();
        size_t ins = 0, outs = 0;
        for (auto& p : ports) {
            if (p.is_input)
                ++ins;
            else
                ++outs;
            p.resolved = p.desired;
        }
        bool arity = (i == 0) ? (ins == 0 && outs == 1)
                     : (i + 1 == n) ? (ins == 1 && outs == 0)
                                    : (ins == 1 && outs == 1);
        if (!arity)
            return fail_step(i, Status::Internal, "node arity mismatch");
        g->ports_.push_back(std::move(ports));
        g->nodes_.push_back(std::move(node));
    }

    for (size_t i = 0; i + 1 < n; ++i) {
        std::unique_ptr<FrameQueue> q(
            new (std::nothrow) FrameQueue(req.queue_capacity));
        if (!q)
            return fail_step(i + 1, Status::Nomem, "queue alloc failed");
        g->edges_.push_back(std::move(q));
    }
    for (size_t i = 0; i + 1 < n; ++i) {
        Port* outp = nullptr;
        Port* inp = nullptr;
        for (auto& p : g->ports_[i])
            if (!p.is_input)
                outp = &p;
        for (auto& p : g->ports_[i + 1])
            if (p.is_input)
                inp = &p;
        auto r = unify(outp->resolved, inp->resolved, diag);
        if (!r)
            return fail_chain(i + 1, r.status(), r.diag());
        outp->resolved = r.value();
        inp->resolved = r.value();
    }
    for (size_t i = 0; i < n; ++i) {
        Diag d;
        Status st = g->nodes_[i]->configure(g->ports_[i], &d);
        if (st != Status::Ok) {
            if (diag)
                diag->add("build", std::string(g->nodes_[i]->id()),
                          "configure rejected");
            return fail_chain(i, st, diag ? *diag : d);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (!g->nodes_[i]->wants_model())
            continue;
        const Model* m = req.model_id.empty() ? ctx.first_model()
                                              : ctx.find_model(req.model_id);
        if (!m)
            return fail_step(i, Status::Negotiate, "no compatible model");
        Diag d;
        Status st = g->nodes_[i]->bind_model(*m, &d);
        if (st != Status::Ok)
            return fail_chain(i, st, diag ? *diag : d);
    }
    for (size_t i = 0; i < n; ++i) {
        std::vector<FrameQueue*> outs;
        if (i + 1 < n)
            outs.push_back(g->edges_[i].get());
        std::unique_ptr<Emit> e(new (std::nothrow) NodeEmit(std::move(outs)));
        if (!e)
            return fail_step(i, Status::Nomem, "emit alloc failed");
        g->emits_.push_back(std::move(e));
    }
    if (failed_step)
        *failed_step = 0;
    return Result<std::unique_ptr<Graph>>::success(std::move(g));
}

Status Graph::open(Diag* diag) {
    for (size_t i = 0; i < nodes_.size(); ++i) {
        Status st = nodes_[i]->open(emits_[i].get(), diag);
        if (st != Status::Ok) {
            failure_step_ = i;
            for (size_t j = i; j-- > 0;)
                nodes_[j]->close();
            return st;
        }
    }
    return Status::Ok;
}

void Graph::close_nodes() noexcept {
    for (size_t i = nodes_.size(); i-- > 0;)
        nodes_[i]->close();
}

}  // namespace rkvc
