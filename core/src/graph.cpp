// SPDX-License-Identifier: AGPL-3.0-or-later
#include "rkvc/graph.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <utility>

#include <sys/mman.h>

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

namespace {

// CPU download bridge: linear Dmabuf in -> packed Host out. Lets Dmabuf
// producers (e.g. the MPP decoder) feed Host consumers; build() splices one
// in wherever a Dmabuf -> Host edge would otherwise fail negotiation.
class DownloadNode : public Node {
public:
    std::string_view id() const noexcept override { return "core.download"; }
    std::vector<Port> make_ports() const override {
        Port in, out;
        in.name = "in";
        in.is_input = true;
        in.desired.domain = MemDomain::Dmabuf;
        out.name = "out";
        out.desired.domain = MemDomain::Host;
        return {in, out};
    }
    Status configure(std::vector<Port>& ports, Diag* diag) override {
        Status st = Node::configure(ports, diag);
        if (st != Status::Ok)
            return st;
        if (ports.size() != 2 || !ports[0].is_input || ports[1].is_input) {
            if (diag)
                diag->add("configure", "core.download", "bad port layout");
            return Status::Internal;
        }
        in_ = ports[0].resolved;
        out_ = ports[1].resolved;
        if (in_.domain != MemDomain::Dmabuf || out_.domain != MemDomain::Host ||
            in_.fmt == PixelFormat::Unknown ||
            out_.fmt == PixelFormat::Unknown || in_.fmt != out_.fmt ||
            !is_linear(in_)) {
            if (diag)
                diag->add("configure", "core.download", "unresolvable ports");
            return Status::Negotiate;
        }
        return Status::Ok;
    }
    Status open(Emit* emit, Diag* diag) override {
        if (!emit) {
            if (diag)
                diag->add("open", "core.download", "null emit");
            return Status::Invalid;
        }
        emit_ = emit;
        return Status::Ok;
    }
    Status process(FramePtr input, Diag* diag) override {
        if (!input)
            return fail(diag, Status::Invalid, "null frame");
        const Spec& s = input->spec();
        if (s.domain != MemDomain::Dmabuf || input->fd() < 0)
            return fail(diag, Status::Invalid, "not a dmabuf frame");
        if (s.fmt == PixelFormat::Unknown)
            return fail(diag, Status::Invalid, "unresolved format");
        if (!is_linear(s))
            return fail(diag, Status::Unsupported, "non-linear modifier");
        const size_t mapped = input->size();
        if (mapped == 0)
            return fail(diag, Status::Invalid, "empty dmabuf");
        // NB: dma-buf exporters reject MAP_PRIVATE; read-only MAP_SHARED is
        // safe here because the mapping is never written through.
        void* map = mmap(nullptr, mapped, PROT_READ, MAP_SHARED, input->fd(), 0);
        if (map == MAP_FAILED)
            return fail(diag, Status::Hw, "mmap failed");
        Status st = copy(input, s, map, mapped, diag);
        munmap(map, mapped);
        return st;
    }

private:
    static Status fail(Diag* diag, Status s, const char* reason) {
        if (diag)
            diag->add("process", "core.download", reason);
        return s;
    }
    static void free_payload(void* p) noexcept { std::free(p); }
    // One packed plane descriptor: lines of line_bytes, src plane starts at
    // src_stride * ver_off.
    struct Plane {
        uint32_t lines = 0;
        uint32_t line_bytes = 0;
        uint32_t ver_off = 0;
    };
    Status copy(const FramePtr& input, const Spec& s, const void* map,
                size_t mapped, Diag* diag) {
        if (s.fmt == PixelFormat::Bitstream)
            return finish_flat(input, map, mapped, diag);
        if (s.width == 0 || s.height == 0)
            return fail(diag, Status::Invalid, "unresolved extent");
        if (!even_extent_ok(s))
            return fail(diag, Status::Unsupported,
                        "odd extent for subsampled format");
        Plane planes[3];
        size_t nplanes = 0;
        if (!plan_layout(s, planes, &nplanes))
            return fail(diag, Status::Unsupported, "pixel format not handled");
        Spec d;
        d.width = s.width;
        d.height = s.height;
        d.fmt = s.fmt;
        d.domain = MemDomain::Host;
        const size_t need = min_size(d);
        if (need == 0)
            return fail(diag, Status::Unsupported, "pixel format not handled");
        void* dst = std::malloc(need);
        if (!dst)
            return fail(diag, Status::Nomem, "host alloc failed");
        const uint8_t* src = static_cast<const uint8_t*>(map);
        uint8_t* out = static_cast<uint8_t*>(dst);
        const uint32_t src_stride = eff_stride(s);
        const uint32_t dst_stride = eff_stride(d);
        size_t dst_off = 0;
        for (size_t p = 0; p < nplanes; ++p) {
            if (planes[p].line_bytes > src_stride) {
                std::free(dst);
                return fail(diag, Status::Invalid, "stride smaller than width");
            }
            const size_t start =
                static_cast<size_t>(src_stride) * planes[p].ver_off;
            const size_t end = start +
                               static_cast<size_t>(planes[p].lines - 1) *
                                   src_stride +
                               planes[p].line_bytes;
            if (end > mapped) {
                std::free(dst);
                return fail(diag, Status::Invalid, "plane exceeds mapping");
            }
            for (uint32_t l = 0; l < planes[p].lines; ++l) {
                std::memcpy(out + dst_off, src + start + (size_t)l * src_stride,
                            planes[p].line_bytes);
                dst_off += dst_stride;
            }
        }
        if (dst_off != need) {
            std::free(dst);
            return fail(diag, Status::Internal, "plane accounting mismatch");
        }
        return finish(input, d, dst, need, diag);
    }
    // min_size() sizes subsampled formats as plane*3/2 truncated, which only
    // matches real NV12-family layouts for even extents. Refuse the rest:
    // hardware decoders always emit even (aligned) dimensions anyway.
    static bool even_extent_ok(const Spec& s) noexcept {
        switch (s.fmt) {
            case PixelFormat::Nv12:
            case PixelFormat::Nv21:
            case PixelFormat::P010:
                return (s.width % 2 == 0) && (s.height % 2 == 0);
            case PixelFormat::Nv16:
                return (s.width % 2 == 0);
            default:
                return true;
        }
    }
    // Packed plane layout for the pixel formats MPP-class decoders emit.
    // Yuv420P needs per-plane strides the Spec cannot express; refuse it.
    static bool plan_layout(const Spec& s, Plane* planes, size_t* n) noexcept {
        const uint32_t w = s.width;
        const uint32_t h = s.height;
        const uint32_t uv_h = (h + 1) / 2;
        switch (s.fmt) {
            case PixelFormat::Nv12:
            case PixelFormat::Nv21:
            case PixelFormat::P010: {
                const uint32_t b = (s.fmt == PixelFormat::P010) ? 2 : 1;
                planes[0] = {h, w * b, 0};
                planes[1] = {uv_h, w * b, eff_ver_stride(s)};
                *n = 2;
                return true;
            }
            case PixelFormat::Nv16:
                planes[0] = {h, w, 0};
                planes[1] = {h, w, eff_ver_stride(s)};
                *n = 2;
                return true;
            case PixelFormat::Rgb24:
                planes[0] = {h, w * 3, 0};
                *n = 1;
                return true;
            default:
                return false;
        }
    }
    Status finish_flat(const FramePtr& input, const void* map,
                       size_t mapped, Diag* diag) {
        void* dst = std::malloc(mapped);
        if (!dst)
            return fail(diag, Status::Nomem, "host alloc failed");
        std::memcpy(dst, map, mapped);
        Spec d;
        d.fmt = PixelFormat::Bitstream;
        d.domain = MemDomain::Host;
        return finish(input, d, dst, mapped, diag);
    }
    Status finish(const FramePtr& input, const Spec& d, void* dst, size_t size,
                  Diag* diag) {
        FrameHooks hooks;
        hooks.release = free_payload;
        hooks.ctx = dst;
        auto r = Frame::borrow_host(d, dst, size, hooks, diag);
        if (!r) {
            std::free(dst);
            if (diag)
                diag->add("process", "core.download", "host wrap failed");
            return r.status();
        }
        FramePtr out = std::move(r.value());
        out->set_pts(input->pts());
        out->set_dts(input->dts());
        out->set_flags(input->flags());
        return emit_->emit(0, std::move(out));
    }

    Spec in_;
    Spec out_;
    Emit* emit_ = nullptr;
};

// True when a Dmabuf producer faces a Host consumer and everything else
// already agrees: the only conflict is the memory domain.
bool needs_download_bridge(const Spec& out, const Spec& in) noexcept {
    if (out.domain != MemDomain::Dmabuf || in.domain != MemDomain::Host)
        return false;
    if (out.fmt == PixelFormat::Unknown || !is_linear(out))
        return false;
    Spec probe = out;
    probe.domain = MemDomain::Host;
    return static_cast<bool>(unify(probe, in, nullptr));
}

const Port* last_output(const std::vector<Port>& ports) noexcept {
    for (size_t i = ports.size(); i-- > 0;)
        if (!ports[i].is_input)
            return &ports[i];
    return nullptr;
}

const Port* first_input(const std::vector<Port>& ports) noexcept {
    for (const auto& p : ports)
        if (p.is_input)
            return &p;
    return nullptr;
}

}  // namespace

NodePtr make_download_node() {
    return NodePtr(new (std::nothrow) DownloadNode());
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

    // Dmabuf -> Host bridge: splice a download node wherever a Dmabuf
    // producer would otherwise face a Host consumer with no other conflict.
    for (size_t i = 0; i + 1 < g->nodes_.size(); ++i) {
        const Port* outp = last_output(g->ports_[i]);
        const Port* inp = first_input(g->ports_[i + 1]);
        if (!outp || !inp)
            return fail_step(i + 1, Status::Internal, "missing edge port");
        if (!needs_download_bridge(outp->resolved, inp->resolved))
            continue;
        NodePtr bridge = make_download_node();
        if (!bridge)
            return fail_step(i + 1, Status::Nomem, "download alloc failed");
        std::vector<Port> bports = bridge->make_ports();
        for (auto& p : bports)
            p.resolved = p.desired;
        g->nodes_.insert(g->nodes_.begin() + (i + 1), std::move(bridge));
        g->ports_.insert(g->ports_.begin() + (i + 1), std::move(bports));
        ++i;  // skip the bridge itself
    }
    n = g->nodes_.size();

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
