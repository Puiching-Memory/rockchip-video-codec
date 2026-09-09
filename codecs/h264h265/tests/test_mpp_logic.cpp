// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "h264h265/detail.hpp"

TEST_CASE("annexb sniffing") {
    using h264h265::detail::SniffedCodec;
    using h264h265::detail::sniff_annexb;
    // H.264 SPS (nal type 7) behind a 3-byte start code.
    const uint8_t h264[] = {0, 0, 1, 0x67, 0x42, 0x00, 0x1e, 0, 0, 1, 0x65};
    CHECK(sniff_annexb(h264, sizeof(h264)) == SniffedCodec::Avc);
    // HEVC VPS (nal type 32) behind a 4-byte start code.
    const uint8_t hevc[] = {0, 0, 0, 1, 0x40, 0x01, 0x0c, 0x01};
    CHECK(sniff_annexb(hevc, sizeof(hevc)) == SniffedCodec::Hevc);
    // No parameter sets.
    const uint8_t none[] = {0, 0, 1, 0x65, 0x11, 0x22};
    CHECK(sniff_annexb(none, sizeof(none)) == SniffedCodec::Unknown);
    CHECK(sniff_annexb(nullptr, 0) == SniffedCodec::Unknown);
    const uint8_t tiny[] = {0, 0, 1};
    CHECK(sniff_annexb(tiny, sizeof(tiny)) == SniffedCodec::Unknown);
}

TEST_CASE("roi clamping") {
    using h264h265::detail::clamp_roi;
    rkvc::RoiRegion r;
    r.x = 10;
    r.y = 20;
    r.width = 100;
    r.height = 60;
    r.qp_delta = -8;
    r.force_intra = true;
    auto ok = clamp_roi(&r, 1, 640, 480);
    CHECK(ok);
    if (ok) {
        // left=0 (10&~15), top=16, right=112->112, bottom=80.
        CHECK(ok.value()[0].x == 0);
        CHECK(ok.value()[0].y == 16);
        CHECK(ok.value()[0].w == 112);
        CHECK(ok.value()[0].h == 64);
        CHECK(ok.value()[0].qp_delta == -8);
        CHECK(ok.value()[0].force_intra);
    }
    // Clipped at the frame edge.
    rkvc::RoiRegion edge = r;
    edge.x = 600;
    edge.width = 100;
    auto clipped = clamp_roi(&edge, 1, 640, 480);
    CHECK(clipped);
    if (clipped)
        CHECK(clipped.value()[0].w == 640 - 592);
    // Empty region is rejected.
    rkvc::RoiRegion empty = {};
    CHECK(clamp_roi(&empty, 1, 640, 480).status() == rkvc::Status::Format);
    // Over-count is rejected.
    std::vector<rkvc::RoiRegion> many(9);
    CHECK(clamp_roi(many.data(), many.size(), 640, 480).status() ==
          rkvc::Status::Invalid);
}
