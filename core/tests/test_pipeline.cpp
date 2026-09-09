// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory>
#include <vector>

#include "rkvc/context.hpp"
#include "rkvc/session.hpp"

using namespace rkvc;

namespace {

Spec nv12_64x32() {
    Spec s;
    s.width = 64;
    s.height = 32;
    s.fmt = PixelFormat::Nv12;
    return s;
}

Spec bitstream() {
    Spec s;
    s.fmt = PixelFormat::Bitstream;
    return s;
}

FramePtr make_frame(int64_t pts) {
    static std::vector<std::vector<uint8_t>> store;
    store.emplace_back(min_size(nv12_64x32()), static_cast<uint8_t>(pts));
    auto& buf = store.back();
    auto r = Frame::borrow_host(nv12_64x32(), buf.data(), buf.size());
    if (!r)
        return nullptr;
    r.value()->set_pts(pts);
    return r.value();
}

class FakePipe : public Node {
public:
    std::string_view id() const noexcept override { return "fake.pipe"; }
    std::vector<Port> make_ports() const override {
        Port in, out;
        in.name = "in";
        in.is_input = true;
        out.name = "out";
        return {in, out};
    }
    Status open(Emit* emit, Diag* diag) override {
        if (!emit) {
            if (diag)
                diag->add("open", "fake.pipe", "null emit");
            return Status::Invalid;
        }
        emit_ = emit;
        return Status::Ok;
    }
    Status process(FramePtr input, Diag* diag) override {
        if (!input) {
            if (diag)
                diag->add("process", "fake.pipe", "null frame");
            return Status::Invalid;
        }
        if (input->pts() == kPoisonPts) {
            if (diag)
                diag->add("process", "fake.pipe", "poison frame");
            return Status::Io;
        }
        return emit_->emit(0, std::move(input));
    }

    static constexpr int64_t kPoisonPts = INT64_MIN + 1;

private:
    Emit* emit_ = nullptr;
};

struct FakeFactory : public Factory {
    FakeFactory(std::string id, int prio, Status create_status)
        : id_(std::move(id)), prio_(prio), create_status_(create_status) {}
    std::string_view id() const noexcept override { return id_; }
    NodeStage stage() const noexcept override { return NodeStage::Encode; }
    int priority() const noexcept override { return prio_; }
    bool matches(const Request&, const DeviceCaps&) const noexcept override {
        return true;
    }
    Result<NodePtr> create(const Request&, Diag* diag) const override {
        if (create_status_ != Status::Ok) {
            if (diag)
                diag->add("create", id_, "injected failure");
            return Result<NodePtr>::failure(create_status_,
                                            diag ? *diag : Diag{});
        }
        NodePtr n(new (std::nothrow) FakePipe());
        if (!n)
            return Result<NodePtr>::failure(Status::Nomem);
        return Result<NodePtr>::success(std::move(n));
    }

    std::string id_;
    int prio_;
    Status create_status_;
};

class FlakyNode : public FakePipe {
public:
    std::string_view id() const noexcept override { return "fake.flaky"; }
    Status open(Emit* emit, Diag* diag) override {
        if (fail_open_) {
            if (diag)
                diag->add("open", "fake.flaky", "no device");
            return Status::Hw;
        }
        return FakePipe::open(emit, diag);
    }
    bool fail_open_ = true;
};

struct FlakyFactory : public Factory {
    std::string_view id() const noexcept override { return "fake.flaky"; }
    NodeStage stage() const noexcept override { return NodeStage::Encode; }
    int priority() const noexcept override { return 50; }
    bool matches(const Request&, const DeviceCaps&) const noexcept override {
        return true;
    }
    Result<NodePtr> create(const Request&, Diag*) const override {
        auto n = std::unique_ptr<FlakyNode>(new (std::nothrow) FlakyNode());
        if (!n)
            return Result<NodePtr>::failure(Status::Nomem);
        n->fail_open_ = first_;
        first_ = false;
        NodePtr out = std::move(n);
        return Result<NodePtr>::success(std::move(out));
    }
    mutable bool first_ = true;
};

Request encode_req() {
    Request r;
    r.operation = Operation::Encode;
    r.input_spec = nv12_64x32();
    r.output_spec = bitstream();
    return r;
}

void add_pipe(Context& ctx, const char* id, int prio,
              Status create_status = Status::Ok) {
    auto f = std::unique_ptr<FakeFactory>(
        new FakeFactory(id, prio, create_status));
    CHECK(ctx.registry().add(std::move(f)) == Status::Ok);
}

}  // namespace

TEST_CASE("no candidate fails planning") {
    Context ctx;
    auto s = Session::create(ctx, encode_req());
    CHECK(!s);
    CHECK(s.status() == Status::NotFound);
}

TEST_CASE("encode roundtrip with eos") {
    Context ctx;
    add_pipe(ctx, "fake.h264", 10);
    auto s = Session::create(ctx, encode_req());
    CHECK(s);
    auto session = s.value();
    CHECK(session->push(make_frame(0)) == Status::Ok);
    CHECK(session->push(make_frame(1)) == Status::Ok);
    CHECK(session->start() == Status::Ok);
    CHECK(session->push(make_frame(2)) == Status::Ok);
    CHECK(session->push_eos() == Status::Ok);
    CHECK(session->push(make_frame(3)) == Status::Eof);
    for (int64_t want : {0, 1, 2}) {
        auto f = session->pull();
        CHECK(f);
        if (f) {
            CHECK(f.value()->pts() == want);
            CHECK(f.value()->data() != nullptr);
        }
    }
    CHECK(session->pull().status() == Status::Eof);
    CHECK(session->try_pull().status() == Status::Eof);
    CHECK(session->close() == Status::Ok);
}

TEST_CASE("backpressure before start") {
    Context ctx;
    add_pipe(ctx, "fake.h264", 10);
    Request r = encode_req();
    r.queue_capacity = 4;
    auto s = Session::create(ctx, r);
    CHECK(s);
    auto session = s.value();
    for (int i = 0; i < 4; ++i)
        CHECK(session->push(make_frame(i)) == Status::Ok);
    CHECK(session->push(make_frame(9)) == Status::Again);
    CHECK(session->try_pull().status() == Status::Again);
    CHECK(session->start() == Status::Ok);
    CHECK(session->push_eos() == Status::Ok);
    int count = 0;
    for (;;) {
        auto f = session->pull();
        if (!f) {
            CHECK(f.status() == Status::Eof);
            break;
        }
        ++count;
    }
    CHECK(count == 4);
}

TEST_CASE("build fallback skips broken candidate") {
    Context ctx;
    add_pipe(ctx, "fake.broken", 99, Status::Internal);
    add_pipe(ctx, "fake.h264", 10);
    auto s = Session::create(ctx, encode_req());
    CHECK(s);
    CHECK(s.value()->start() == Status::Ok);
    CHECK(s.value()->push(make_frame(0)) == Status::Ok);
    CHECK(s.value()->push_eos() == Status::Ok);
    auto f = s.value()->pull();
    CHECK(f);
    CHECK(f.value()->pts() == 0);
    CHECK(s.value()->pull().status() == Status::Eof);
}

TEST_CASE("open fallback retries next candidate") {
    Context ctx;
    auto flaky = std::unique_ptr<FlakyFactory>(new FlakyFactory());
    CHECK(ctx.registry().add(std::move(flaky)) == Status::Ok);
    add_pipe(ctx, "fake.h264", 10);
    auto s = Session::create(ctx, encode_req());
    CHECK(s);
    // create() took the priority-50 flaky node; start() must survive its
    // Hw open failure by falling back to fake.h264.
    CHECK(s.value()->start() == Status::Ok);
    CHECK(s.value()->push(make_frame(5)) == Status::Ok);
    CHECK(s.value()->push_eos() == Status::Ok);
    auto f = s.value()->pull();
    CHECK(f);
    CHECK(f.value()->pts() == 5);
    CHECK(s.value()->pull().status() == Status::Eof);
}

TEST_CASE("process error surfaces instead of eof") {
    Context ctx;
    add_pipe(ctx, "fake.h264", 10);
    auto s = Session::create(ctx, encode_req());
    CHECK(s);
    if (!s)
        return;
    auto session = s.value();
    CHECK(session->start() == Status::Ok);
    CHECK(session->push(make_frame(0)) == Status::Ok);
    CHECK(session->push(make_frame(FakePipe::kPoisonPts)) == Status::Ok);
    CHECK(session->push_eos() == Status::Ok);
    auto first = session->pull();
    CHECK(first);
    if (!first)
        return;
    CHECK(first.value()->pts() == 0);
    auto second = session->pull();
    CHECK(!second);
    CHECK(second.status() == Status::Io);
}
