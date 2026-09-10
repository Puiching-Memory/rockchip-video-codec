// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "rkvc/context.hpp"
#include "rkvc/session.hpp"
#include "sr/post.hpp"
#include "sr/upscale.hpp"

TEST_CASE("phase pack layout") {
    // 2x2 NV12 -> 1x1 core, 12 channels, hand-computed.
    uint8_t y[4] = {10, 20, 30, 40};
    uint8_t uv[2] = {50, 60};  // one interleaved pair
    std::vector<uint8_t> phases(12, 0);
    CHECK(sr::post::phase_pack_nv12(y, 2, uv, 2, 2, 2, phases.data(),
                                    phases.size()) == rkvc::Status::Ok);
    // channel c*4+dy*2+dx: Y(0): {10,20,30,40}, U(1): all 50, V(2): all 60.
    CHECK(phases[0] == 10);
    CHECK(phases[1] == 20);
    CHECK(phases[2] == 30);
    CHECK(phases[3] == 40);
    for (int k = 4; k < 8; ++k)
        CHECK(phases[k] == 50);
    for (int k = 8; k < 12; ++k)
        CHECK(phases[k] == 60);
    CHECK(sr::post::phase_pack_nv12(y, 2, uv, 2, 3, 2, phases.data(),
                                    phases.size()) ==
          rkvc::Status::Invalid);  // odd width
}

TEST_CASE("bicubic constant and identity") {
    const uint32_t W = 8, H = 8;
    std::vector<uint8_t> src(W * H * 3 / 2, 100);
    std::vector<uint8_t> dst(3 * (W * 3) * (H * 3) / 2, 0);
    CHECK(sr::post::bicubic_nv12(src.data(), W, H, W, H, dst.data(), W * 3,
                                 H * 3) == rkvc::Status::Ok);
    for (uint8_t v : dst)
        CHECK(v == 100);
    // Same-size upscale preserves a constant frame bit-exactly.
    std::vector<uint8_t> same(W * H * 3 / 2, 0);
    CHECK(sr::post::bicubic_nv12(src.data(), W, H, W, H, same.data(), W, H) ==
          rkvc::Status::Ok);
    for (uint8_t v : same)
        CHECK(v == 100);
}

TEST_CASE("residual add identity and hand check") {
    const uint32_t rw = 2, rh = 2, W = 12, H = 12;
    std::vector<uint8_t> img(W * H * 3 / 2, 50);
    std::vector<float> res(108 * rw * rh, 0.0f);
    CHECK(sr::post::add_phase_residual(res.data(), rw, rh, img.data(), W, H) ==
          rkvc::Status::Ok);
    for (uint8_t v : img)
        CHECK(v == 50);
    // Single +10.0 residual tap on Y phase (ch0, dy0, dx0) hits pixel (0,0).
    size_t plane = rw * rh;
    res[(0 * 36 + 0 * 6 + 0) * plane + 0] = 10.0f;
    CHECK(sr::post::add_phase_residual(res.data(), rw, rh, img.data(), W, H) ==
          rkvc::Status::Ok);
    CHECK(img[0] == 60);
    CHECK(img[1] == 50);
    CHECK(sr::post::add_phase_residual(res.data(), rw, rh, img.data(), 10, H) ==
          rkvc::Status::Invalid);
}

TEST_CASE("upscale node validation without runtime") {
    rkvc::Context ctx;
    auto f = std::unique_ptr<sr::SrUpscaleFactory>(new sr::SrUpscaleFactory());
    CHECK(ctx.registry().add(std::move(f)) == rkvc::Status::Ok);
    // No model registered: build淘汰 with Negotiate (wants_model, none).
    rkvc::Request r;
    r.operation = rkvc::Operation::Upscale;
    rkvc::Spec in;
    in.width = 64;
    in.height = 64;
    in.fmt = rkvc::PixelFormat::Nv12;
    r.input_spec = in;
    r.output_spec = in;
    auto s = rkvc::Session::create(ctx, r);
    CHECK(!s);
    if (!s)
        CHECK(s.status() == rkvc::Status::Negotiate);
    // With a model but no RKNN runtime (container): open fails Hw.
    rkvc::Model m;
    m.meta.id = "sr-test";
    m.meta.family = "sr";
    m.meta.role = "upscale";
    rkvc::ModelPayload p;
    p.kind = "rknn";
    p.data = {7, 7, 7};
    m.payloads = {p};
    CHECK(ctx.add_model(std::move(m)) == rkvc::Status::Ok);
    // UPSCALE plan also matches the core file adapters only for FILE
    // endpoints; FrameSink needs queue adapters (builtin). The session
    // builds, then start() must fail Hw without a runtime.
    auto s2 = rkvc::Session::create(ctx, r);
    if (s2)
        CHECK(s2.value()->start() == rkvc::Status::Hw);
}

TEST_CASE("sr plugin registers its factory") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_SR_PLUGIN) == rkvc::Status::Ok);
    CHECK(ctx.plugin_count() == 1);
    CHECK(ctx.registry().find("rknn.upscale") != nullptr);
}
