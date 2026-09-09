// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "rkvc/context.hpp"
#include "rkvc/session.hpp"

namespace {

rkvc::Spec nv12_32() {
    rkvc::Spec s;
    s.width = 64;
    s.height = 64;
    s.fmt = rkvc::PixelFormat::Nv12;
    return s;
}

rkvc::Request av1_req() {
    rkvc::Request r;
    r.operation = rkvc::Operation::Encode;
    r.codec = rkvc::Codec::Av1;
    r.input_spec = nv12_32();
    rkvc::Spec out;
    out.fmt = rkvc::PixelFormat::Bitstream;
    r.output_spec = out;
    return r;
}

}  // namespace

TEST_CASE("svt av1 software encode end to end") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_AV1_PLUGIN) == rkvc::Status::Ok);
    auto s = rkvc::Session::create(ctx, av1_req());
    CHECK(s);
    if (!s)
        return;
    auto session = s.value();
    rkvc::Diag diag;
    CHECK(session->start(&diag) == rkvc::Status::Ok);
    if (!diag.empty())
        printf("start diag: %s", diag.format().c_str());
    std::vector<uint8_t> yuv(64 * 64 * 3 / 2, 128);
    for (int i = 0; i < 5; ++i) {
        auto fr = rkvc::Frame::borrow_host(nv12_32(), yuv.data(), yuv.size());
        CHECK(fr);
        if (!fr)
            return;
        fr.value()->set_pts(i * 3000);
        CHECK(session->push(fr.value()) == rkvc::Status::Ok);
    }
    CHECK(session->push_eos() == rkvc::Status::Ok);
    size_t bytes = 0;
    int packets = 0;
    bool saw_keyframe = false;
    for (;;) {
        auto f = session->pull();
        if (!f) {
            if (f.status() != rkvc::Status::Eof)
                printf("pull failed: %d\n", (int)f.status());
            CHECK(f.status() == rkvc::Status::Eof);
            break;
        }
        ++packets;
        bytes += f.value()->size();
        if (f.value()->flags() & rkvc::kFlagKeyframe)
            saw_keyframe = true;
    }
    CHECK(packets >= 1);
    CHECK(bytes > 0);
    CHECK(saw_keyframe);
}

TEST_CASE("svt rejects rgb input at negotiate") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_AV1_PLUGIN) == rkvc::Status::Ok);
    rkvc::Request r = av1_req();
    r.input_spec.fmt = rkvc::PixelFormat::Rgb24;
    auto s = rkvc::Session::create(ctx, r);
    CHECK(!s);
    if (!s)
        CHECK(s.status() == rkvc::Status::Negotiate);
}

TEST_CASE("av1 plugin registers its factory") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_AV1_PLUGIN) == rkvc::Status::Ok);
    CHECK(ctx.plugin_count() == 1);
    CHECK(ctx.registry().find("svt.encode") != nullptr);
}
