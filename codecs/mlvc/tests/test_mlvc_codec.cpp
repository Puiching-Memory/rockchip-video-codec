// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "mlvc/codec.hpp"
#include "rkvc/context.hpp"
#include "rkvc/session.hpp"

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put_f64(std::vector<uint8_t>& v, double d) {
    uint64_t bits = 0;
    memcpy(&bits, &d, 8);
    put32(v, static_cast<uint32_t>(bits));
    put32(v, static_cast<uint32_t>(bits >> 32));
}

// 8 dists x {64,64,64,32,32}, gaussian tag.
std::vector<uint8_t> make_gaussian() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'P', 'M', 'F', '1'});
    put32(b, 8);
    put32(b, 8);
    put32(b, 40);
    for (int i = 0; i < 8; ++i)
        put32(b, 5);  // lengths: 5 symbols each
    for (int i = 0; i < 8; ++i)
        put32(b, 0);  // offsets
    for (int i = 0; i < 40; ++i)
        put32(b, (i % 5 == 3 || i % 5 == 4) ? 32 : 64);
    put32(b, 1);
    put_f64(b, 0.5);
    put_f64(b, 2.5);
    put32(b, 8);
    put32(b, 16);
    return b;
}

// 512 dists x {64,64,64,32,32}, bitest tag (qp_num=64, channels=8).
std::vector<uint8_t> make_bitest() {
    std::vector<uint8_t> b;
    b.insert(b.end(), {'P', 'M', 'F', '1'});
    put32(b, 512);
    put32(b, 512);
    put32(b, 2560);
    for (int i = 0; i < 512; ++i)
        put32(b, 5);  // lengths: 5 symbols each
    for (int i = 0; i < 512; ++i)
        put32(b, 0);  // offsets
    for (int i = 0; i < 2560; ++i)
        put32(b, (i % 5 == 3 || i % 5 == 4) ? 32 : 64);
    put32(b, 2);
    put32(b, 64);
    put32(b, 8);
    return b;
}

rkvc::Model make_model(const std::string& id, const std::string& role) {
    rkvc::Model m;
    m.meta.id = id;
    m.meta.family = "mlvc";
    m.meta.role = role;
    m.meta.target = "fake";
    rkvc::ModelPayload rknn;
    rknn.kind = "rknn";
    rknn.data = {1, 2, 3, 4, 5, 6, 7, 8};
    rkvc::ModelPayload g;
    g.kind = "pmf-gaussian";
    g.data = make_gaussian();
    rkvc::ModelPayload bi;
    bi.kind = "pmf-bitest";
    bi.data = make_bitest();
    m.payloads = {rknn, g, bi};
    return m;
}

rkvc::Spec nv12_64() {
    rkvc::Spec s;
    s.width = 64;
    s.height = 64;
    s.fmt = rkvc::PixelFormat::Nv12;
    return s;
}

std::vector<uint8_t> white_nv12() {
    std::vector<uint8_t> v(64 * 64 * 3 / 2, 128);
    for (int i = 0; i < 64 * 64; ++i)
        v[i] = 255;
    return v;
}

void register_fake(rkvc::Context& ctx) {
    auto enc = std::unique_ptr<mlvc::MlvcEncodeFactory>(
        new mlvc::MlvcEncodeFactory([]() -> std::unique_ptr<mlvc::NpuModel> {
            return mlvc::make_fake_encoder();
        }));
    CHECK(ctx.registry().add(std::move(enc)) == rkvc::Status::Ok);
    auto dec = std::unique_ptr<mlvc::MlvcDecodeFactory>(
        new mlvc::MlvcDecodeFactory([]() -> std::unique_ptr<mlvc::NpuModel> {
            return mlvc::make_fake_decoder();
        }));
    CHECK(ctx.registry().add(std::move(dec)) == rkvc::Status::Ok);
}

}  // namespace

TEST_CASE("mlvc plugin loads without rknn") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_MLVC_PLUGIN) == rkvc::Status::Ok);
    CHECK(ctx.plugin_count() == 1);
    CHECK(ctx.registry().find("mlvc.encode") != nullptr);
    CHECK(ctx.registry().find("mlvc.decode") != nullptr);
}

TEST_CASE("mlvc fake roundtrip") {
    rkvc::Context ctx;
    register_fake(ctx);
    CHECK(ctx.add_model(make_model("fake-enc", "encoder")) == rkvc::Status::Ok);
    CHECK(ctx.add_model(make_model("fake-dec", "decoder")) == rkvc::Status::Ok);

    rkvc::Request er;
    er.operation = rkvc::Operation::Encode;
    er.codec = rkvc::Codec::Mlvc;
    er.input_spec = nv12_64();
    rkvc::Spec bs;
    bs.fmt = rkvc::PixelFormat::Bitstream;
    er.output_spec = bs;
    er.model_id = "fake-enc";
    er.quality.bitrate_bps = 200000;  // exercise the rate controller
    auto es = rkvc::Session::create(ctx, er);
    CHECK(es);
    if (!es)
        return;
    auto enc = es.value();
    CHECK(enc->start() == rkvc::Status::Ok);
    auto yuv = white_nv12();
    for (int i = 0; i < 4; ++i) {
        auto fr = rkvc::Frame::borrow_host(nv12_64(), yuv.data(), yuv.size());
        CHECK(fr);
        if (!fr)
            return;
        fr.value()->set_pts(i * 3000);
        CHECK(enc->push(fr.value()) == rkvc::Status::Ok);
    }
    CHECK(enc->push_eos() == rkvc::Status::Ok);
    std::vector<uint8_t> stream;
    int packets = 0;
    bool first_big = false;
    for (;;) {
        auto f = enc->pull();
        if (!f) {
            CHECK(f.status() == rkvc::Status::Eof);
            break;
        }
        if (packets == 0)
            first_big = f.value()->size() > 64 + 16;
        const uint8_t* p = static_cast<const uint8_t*>(f.value()->data());
        stream.insert(stream.end(), p, p + f.value()->size());
        ++packets;
    }
    CHECK(packets == 4);
    CHECK(first_big);  // first record carries the 64B header
    CHECK(!stream.empty());

    rkvc::Request dr;
    dr.operation = rkvc::Operation::Decode;
    dr.codec = rkvc::Codec::Mlvc;
    dr.input_spec = bs;
    dr.output_spec = nv12_64();
    dr.model_id = "fake-dec";
    auto ds = rkvc::Session::create(ctx, dr);
    CHECK(ds);
    if (!ds)
        return;
    auto dec = ds.value();
    CHECK(dec->start() == rkvc::Status::Ok);
    // Split the stream to exercise the streaming demuxer.
    size_t cut = stream.size() / 2;
    for (size_t off : {size_t{0}, cut}) {
        size_t n = (off == 0) ? cut : stream.size() - cut;
        auto fr = rkvc::Frame::borrow_host(bs, stream.data() + off, n);
        CHECK(fr);
        if (!fr)
            return;
        CHECK(dec->push(fr.value()) == rkvc::Status::Ok);
    }
    CHECK(dec->push_eos() == rkvc::Status::Ok);
    int frames = 0;
    bool key0 = false;
    for (;;) {
        auto f = dec->pull();
        if (!f) {
            CHECK(f.status() == rkvc::Status::Eof);
            break;
        }
        ++frames;
        if (frames == 1)
            key0 = (f.value()->flags() & rkvc::kFlagKeyframe) != 0;
        CHECK(f.value()->spec().width == 64);
        CHECK(f.value()->spec().height == 64);
        CHECK(f.value()->size() == 64 * 64 * 3 / 2);
        const uint8_t* p = static_cast<const uint8_t*>(f.value()->data());
        // White in -> Y reconstructs to 128 through the fake stub.
        for (int i = 0; i < 64 * 64; ++i)
            CHECK(p[i] == 128);
    }
    CHECK(frames == 4);
    CHECK(key0);
}

TEST_CASE("mlvc ltr scheduling runs") {
    rkvc::Context ctx;
    register_fake(ctx);
    CHECK(ctx.add_model(make_model("fake-enc", "encoder")) == rkvc::Status::Ok);
    CHECK(ctx.add_model(make_model("fake-dec", "decoder")) == rkvc::Status::Ok);
    rkvc::Request er;
    er.operation = rkvc::Operation::Encode;
    er.codec = rkvc::Codec::Mlvc;
    er.input_spec = nv12_64();
    rkvc::Spec bs;
    bs.fmt = rkvc::PixelFormat::Bitstream;
    er.output_spec = bs;
    er.model_id = "fake-enc";
    er.queue_capacity = 8;  // 5 pushes without drain; default 4 races the
                            // consumer thread under parallel ctest load.
    er.quality.gop_size = 8;
    er.quality.ltr_period = 2;
    er.quality.ltr_start_idx = 0;
    auto es = rkvc::Session::create(ctx, er);
    CHECK(es);
    if (!es)
        return;
    auto enc = es.value();
    CHECK(enc->start() == rkvc::Status::Ok);
    auto yuv = white_nv12();
    for (int i = 0; i < 5; ++i) {
        auto fr = rkvc::Frame::borrow_host(nv12_64(), yuv.data(), yuv.size());
        CHECK(fr);
        if (!fr)
            return;
        CHECK(enc->push(fr.value()) == rkvc::Status::Ok);
    }
    CHECK(enc->push_eos() == rkvc::Status::Ok);
    std::vector<uint8_t> stream;
    int packets = 0;
    for (;;) {
        auto f = enc->pull();
        if (!f)
            break;
        const uint8_t* p = static_cast<const uint8_t*>(f.value()->data());
        stream.insert(stream.end(), p, p + f.value()->size());
        ++packets;
    }
    CHECK(packets == 5);

    rkvc::Request dr;
    dr.operation = rkvc::Operation::Decode;
    dr.codec = rkvc::Codec::Mlvc;
    dr.input_spec = bs;
    dr.output_spec = nv12_64();
    dr.model_id = "fake-dec";
    auto ds = rkvc::Session::create(ctx, dr);
    CHECK(ds);
    if (!ds)
        return;
    auto dec = ds.value();
    CHECK(dec->start() == rkvc::Status::Ok);
    auto fr = rkvc::Frame::borrow_host(bs, stream.data(), stream.size());
    CHECK(fr);
    if (!fr)
        return;
    CHECK(dec->push(fr.value()) == rkvc::Status::Ok);
    CHECK(dec->push_eos() == rkvc::Status::Ok);
    int frames = 0;
    for (;;) {
        auto f = dec->pull();
        if (!f) {
            CHECK(f.status() == rkvc::Status::Eof);
            break;
        }
        ++frames;
    }
    CHECK(frames == 5);
}
