// SPDX-License-Identifier: AGPL-3.0-or-later
// Plugin load smoke (MPP calls land in the stub): registration works,
// sessions correctly find no candidate without a device.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "rkvc/context.hpp"
#include "rkvc/session.hpp"

TEST_CASE("mpp plugin loads and registers factories") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_MPP_PLUGIN) == rkvc::Status::Ok);
    CHECK(ctx.plugin_count() == 1);
    CHECK(ctx.registry().find("mpp.decode") != nullptr);
    CHECK(ctx.registry().find("mpp.encode") != nullptr);
}

TEST_CASE("no candidate without device") {
    rkvc::Context ctx;
    CHECK(ctx.load_plugin(RKVC_TEST_MPP_PLUGIN) == rkvc::Status::Ok);
    rkvc::Request r;
    r.operation = rkvc::Operation::Encode;
    r.codec = rkvc::Codec::H264;
    rkvc::Spec in;
    in.width = 64;
    in.height = 64;
    in.fmt = rkvc::PixelFormat::Nv12;
    r.input_spec = in;
    rkvc::Spec out;
    out.fmt = rkvc::PixelFormat::Bitstream;
    r.output_spec = out;
    auto s = rkvc::Session::create(ctx, r);
    CHECK(!s);
    if (!s)
        CHECK(s.status() == rkvc::Status::NotFound);
}
