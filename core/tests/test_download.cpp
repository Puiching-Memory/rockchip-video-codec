// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

#include "rkvc/context.hpp"
#include "rkvc/frame.hpp"
#include "rkvc/graph.hpp"
#include "rkvc/node.hpp"
#include "rkvc/registry.hpp"
#include "rkvc/session.hpp"
#include "rkvc/spec.hpp"

using namespace rkvc;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 32;
constexpr uint32_t kStride = 80;  // padded: proves destriding

uint8_t y_at(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>((x + y * 3) & 0xff);
}
uint8_t uv_at(uint32_t x, uint32_t y) {
    return static_cast<uint8_t>(0x80 + ((x + y) & 0x3f));
}

Spec nv12_dmabuf(uint32_t w, uint32_t h, uint32_t stride) {
    Spec s;
    s.width = w;
    s.height = h;
    s.fmt = PixelFormat::Nv12;
    s.domain = MemDomain::Dmabuf;
    s.stride = stride;
    s.ver_stride = h;
    return s;
}

size_t dmabuf_size(uint32_t h, uint32_t stride, uint32_t ver_stride) {
    return (size_t)stride * ver_stride + (size_t)stride * ((h + 1) / 2);
}

// Owns a memfd holding a strided NV12 pattern image.
struct PatternImage {
    int fd = -1;
    size_t size = 0;
    Spec spec;
    ~PatternImage() {
        if (fd >= 0)
            ::close(fd);
    }
    bool build(uint32_t w, uint32_t h, uint32_t stride,
               uint32_t ver_stride = 0) {
        if (ver_stride == 0)
            ver_stride = h;
        spec = nv12_dmabuf(w, h, stride);
        spec.ver_stride = ver_stride;
        size = dmabuf_size(h, stride, ver_stride);
        fd = memfd_create("rkvc-download-test", MFD_CLOEXEC);
        if (fd < 0)
            return false;
        if (::ftruncate(fd, (off_t)size) != 0)
            return false;
        void* p =
            ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED)
            return false;
        auto* px = static_cast<uint8_t*>(p);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                px[(size_t)y * stride + x] = y_at(x, y);
        uint8_t* uv = px + (size_t)stride * ver_stride;
        const uint32_t uv_h = (h + 1) / 2;
        for (uint32_t y = 0; y < uv_h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                uv[(size_t)y * stride + x] = uv_at(x, y);
        ::munmap(p, size);
        return true;
    }
    FramePtr wrap() {
        auto r = Frame::borrow_dmabuf(spec, fd, size, FrameHooks{}, nullptr);
        if (!r)
            return nullptr;
        return std::move(r.value());
    }
};

class Sink : public Emit {
public:
    Status emit(size_t, FramePtr f) override {
        frames.push_back(std::move(f));
        return Status::Ok;
    }
    std::vector<FramePtr> frames;
};

bool is_packed_nv12(const Frame& f, uint32_t w, uint32_t h) {
    if (f.spec().domain != MemDomain::Host ||
        f.spec().fmt != PixelFormat::Nv12 || !f.data())
        return false;
    if (f.size() != (size_t)w * h * 3 / 2)
        return false;
    const auto* px = static_cast<const uint8_t*>(f.data());
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if (px[(size_t)y * w + x] != y_at(x, y))
                return false;
    const uint8_t* uv = px + (size_t)w * h;
    const uint32_t uv_h = (h + 1) / 2;
    for (uint32_t y = 0; y < uv_h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            if (uv[(size_t)y * w + x] != uv_at(x, y))
                return false;
    return true;
}

NodePtr open_download(Sink& sink, const Spec& in_spec) {
    NodePtr node = make_download_node();
    if (!node)
        return nullptr;
    std::vector<Port> ports = node->make_ports();
    if (ports.size() != 2)
        return nullptr;
    // Mirror Graph::build: resolve ports before configure.
    ports[0].resolved = in_spec;
    ports[1].resolved = in_spec;
    ports[1].resolved.domain = MemDomain::Host;
    ports[1].resolved.stride = 0;
    ports[1].resolved.ver_stride = 0;
    if (node->configure(ports, nullptr) != Status::Ok)
        return nullptr;
    if (node->open(&sink, nullptr) != Status::Ok)
        return nullptr;
    return node;
}

}  // namespace

TEST_CASE("download destrides padded nv12 to packed host") {
    PatternImage img;
    CHECK(img.build(kW, kH, kStride));
    Sink sink;
    NodePtr node = open_download(sink, img.spec);
    CHECK(node);
    if (!node) return;
    FramePtr in = img.wrap();
    CHECK(in);
    if (!in) return;
    in->set_pts(123);
    in->set_flags(7);
    CHECK(node->process(std::move(in), nullptr) == Status::Ok);
    CHECK(sink.frames.size() == 1);
    if (sink.frames.size() != 1) return;
    const FramePtr& out = sink.frames[0];
    CHECK(out->spec().width == kW);
    CHECK(out->spec().height == kH);
    CHECK(is_packed_nv12(*out, kW, kH));
    CHECK(out->pts() == 123);
    CHECK(out->flags() == 7u);
}

TEST_CASE("download honors padded ver_stride") {
    // MPP-style alignment: 48x16 picture in a stride-64, ver_stride-24 plane.
    PatternImage img;
    CHECK(img.build(48, 16, 64, 24));
    Sink sink;
    NodePtr node = open_download(sink, img.spec);
    CHECK(node);
    if (!node) return;
    FramePtr in = img.wrap();
    CHECK(in);
    if (!in) return;
    CHECK(node->process(std::move(in), nullptr) == Status::Ok);
    CHECK(sink.frames.size() == 1);
    if (sink.frames.size() != 1) return;
    CHECK(is_packed_nv12(*sink.frames[0], 48, 16));
}

TEST_CASE("download rejects odd height for nv12") {
    PatternImage img;
    CHECK(img.build(6, 5, 8));
    Sink sink;
    NodePtr node = open_download(sink, nv12_dmabuf(6, 4, 8));
    CHECK(node);
    if (!node) return;
    // Odd-height input cannot match min_size's plane*3/2 sizing: refused.
    FramePtr in = img.wrap();
    CHECK(in);
    if (!in) return;
    CHECK(node->process(std::move(in), nullptr) == Status::Unsupported);
    CHECK(sink.frames.empty());
}

TEST_CASE("download copies bitstream payload flat") {
    const char payload[] = "fake-bitstream-payload";
    const size_t len = sizeof(payload);
    int fd = memfd_create("rkvc-download-bs", MFD_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, (off_t)len) == 0);
    void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED);
    std::memcpy(p, payload, len);
    ::munmap(p, len);
    Spec s;
    s.fmt = PixelFormat::Bitstream;
    s.domain = MemDomain::Dmabuf;
    auto r = Frame::borrow_dmabuf(s, fd, len, FrameHooks{}, nullptr);
    CHECK(r);
    Sink sink;
    NodePtr node = open_download(sink, s);
    CHECK(node);
    if (!node) return;
    CHECK(node->process(std::move(r.value()), nullptr) == Status::Ok);
    CHECK(sink.frames.size() == 1);
    if (sink.frames.size() != 1) return;
    const FramePtr& out = sink.frames[0];
    CHECK(out->spec().domain == MemDomain::Host);
    CHECK(out->spec().fmt == PixelFormat::Bitstream);
    CHECK(out->size() == len);
    CHECK(std::memcmp(out->data(), payload, len) == 0);
    ::close(fd);
}

TEST_CASE("download rejects host input") {
    std::vector<uint8_t> buf(min_size(nv12_dmabuf(kW, kH, kW)), 0);
    Spec host = nv12_dmabuf(kW, kH, kW);
    host.domain = MemDomain::Host;
    auto r = Frame::borrow_host(host, buf.data(), buf.size(), FrameHooks{},
                                nullptr);
    CHECK(r);
    Sink sink;
    // Node opens against a Dmabuf spec; the Host frame is rejected at process.
    NodePtr node = open_download(sink, nv12_dmabuf(kW, kH, kW));
    CHECK(node);
    if (!node) return;
    CHECK(node->process(std::move(r.value()), nullptr) == Status::Invalid);
    CHECK(sink.frames.empty());
}

namespace {

// Fake Dmabuf-producing decoder: Host in, Dmabuf Nv12 out.
class DmabufProducer : public Node {
public:
    std::string_view id() const noexcept override { return "fake.dmabuf"; }
    std::vector<Port> make_ports() const override {
        Port in, out;
        in.name = "in";
        in.is_input = true;
        out.name = "out";
        out.desired.fmt = PixelFormat::Nv12;
        out.desired.domain = MemDomain::Dmabuf;
        return {in, out};
    }
    Status open(Emit* emit, Diag* diag) override {
        if (!emit) {
            if (diag)
                diag->add("open", "fake.dmabuf", "null emit");
            return Status::Invalid;
        }
        emit_ = emit;
        if (!img_.build(kW, kH, kStride)) {
            if (diag)
                diag->add("open", "fake.dmabuf", "memfd failed");
            return Status::Hw;
        }
        return Status::Ok;
    }
    Status process(FramePtr input, Diag* diag) override {
        if (!input) {
            if (diag)
                diag->add("process", "fake.dmabuf", "null frame");
            return Status::Invalid;
        }
        FramePtr out = img_.wrap();
        if (!out) {
            if (diag)
                diag->add("process", "fake.dmabuf", "wrap failed");
            return Status::Nomem;
        }
        out->set_pts(input->pts());
        return emit_->emit(0, std::move(out));
    }

private:
    Emit* emit_ = nullptr;
    PatternImage img_;
};

struct ProducerFactory : public Factory {
    std::string_view id() const noexcept override { return "fake.dmabuf"; }
    NodeStage stage() const noexcept override { return NodeStage::Decode; }
    int priority() const noexcept override { return 10; }
    bool matches(const Request&, const DeviceCaps&) const noexcept override {
        return true;
    }
    Result<NodePtr> create(const Request&, Diag*) const override {
        NodePtr n(new (std::nothrow) DmabufProducer());
        if (!n)
            return Result<NodePtr>::failure(Status::Nomem);
        return Result<NodePtr>::success(std::move(n));
    }
};

Request decode_req() {
    Request r;
    r.operation = Operation::Decode;
    r.input_spec = nv12_dmabuf(kW, kH, kW);
    r.input_spec.domain = MemDomain::Host;
    r.output_spec = nv12_dmabuf(kW, kH, kW);
    r.output_spec.domain = MemDomain::Host;
    return r;
}

}  // namespace

TEST_CASE("session auto-splices download between dmabuf and host") {
    Context ctx;
    auto f =
        std::unique_ptr<ProducerFactory>(new (std::nothrow) ProducerFactory());
    CHECK(f);
    CHECK(ctx.registry().add(std::move(f)) == Status::Ok);
    auto s = Session::create(ctx, decode_req());
    CHECK(s);
    auto session = s.value();
    std::vector<uint8_t> buf(min_size(decode_req().input_spec), 0);
    auto fr = Frame::borrow_host(decode_req().input_spec, buf.data(),
                                 buf.size(), FrameHooks{}, nullptr);
    CHECK(fr);
    fr.value()->set_pts(42);
    CHECK(session->push(std::move(fr.value())) == Status::Ok);
    CHECK(session->start() == Status::Ok);
    CHECK(session->push_eos() == Status::Ok);
    auto out = session->pull();
    CHECK(out);
    if (!out) return;
    CHECK(is_packed_nv12(*out.value(), kW, kH));
    CHECK(out.value()->pts() == 42);
    CHECK(session->pull().status() == Status::Eof);
    CHECK(session->close() == Status::Ok);
}
