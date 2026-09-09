// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "mlvc/pixel.hpp"

namespace {

uint16_t ref_f16(float f) {
    // Independent bit-correct f32->f16 (round-to-nearest-even).
    uint32_t x = 0;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = static_cast<int>((x >> 23) & 0xff) - 112;
    uint32_t mant = x & 0x7fffff;
    if (exp <= 0) {
        if (exp < -10)
            return static_cast<uint16_t>(sign);
        mant |= 0x800000;
        uint32_t sh = static_cast<uint32_t>(14 - exp);
        uint32_t m16 = mant >> sh;
        uint32_t rest = mant & ((1u << sh) - 1);
        uint32_t half = 1u << (sh - 1);
        if (rest > half || (rest == half && (m16 & 1)))
            ++m16;
        return static_cast<uint16_t>(sign | (m16 & 0x3ff));
    }
    if (exp >= 31)
        return static_cast<uint16_t>(sign | 0x7c00);
    uint32_t m16 = mant >> 13;
    uint32_t rest = mant & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (m16 & 1)))
        ++m16;
    if (m16 == 0x400) {
        ++exp;
        m16 = 0;
        if (exp >= 31)
            return static_cast<uint16_t>(sign | 0x7c00);
    }
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
                                 m16);
}

}  // namespace

TEST_CASE("f32 f16 converters") {
    using mlvc::pixel::f16_to_f32;
    using mlvc::pixel::f32_to_f16;
    CHECK(f32_to_f16(1.0f) == 0x3c00);
    CHECK(f32_to_f16(-1.0f) == 0xbc00);
    CHECK(f32_to_f16(0.0f) == 0x0000);
    float inf = INFINITY;
    CHECK(f32_to_f16(inf) == 0x7c00);
    // Cross-check the portable implementation against an independent one.
    uint32_t rng = 0xabcdeu;
    for (int i = 0; i < 2000; ++i) {
        rng = rng * 1103515245u + 12345u;
        float f = 0;
        memcpy(&f, &rng, 4);
        if (!std::isfinite(f))
            continue;
        CHECK(f32_to_f16(f) == ref_f16(f));
    }
    // Roundtrip is exact for exactly-representable values.
    for (int i = 0; i < 256; ++i) {
        float f = i / 255.0f;
        CHECK(f16_to_f32(f32_to_f16(f)) == f16_to_f32(ref_f16(f)));
    }
}

TEST_CASE("extract scales checkerboard") {
    using mlvc::pixel::extract_scales;
    // YC=2, 4x4, Z 2x1x1, cr=2, sr=2: both channels see pair (5, -9).
    int32_t z[] = {5, -9};
    std::vector<int32_t> s0(2 * 16, 0), s1(2 * 16, 0);
    CHECK(extract_scales(z, s0.data(), s1.data(), 2, 4, 4, 2, 1, 1, 2, 2,
                         63) == rkvc::Status::Ok);
    for (int c = 0; c < 2; ++c) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                bool even = ((y & 1) == (x & 1));
                CHECK(s0[c * 16 + y * 4 + x] == (even ? 5 : 9));
                CHECK(s1[c * 16 + y * 4 + x] == (even ? 9 : 5));
            }
        }
    }
    // INT32_MIN magnitude clamps safely.
    int32_t zmin[] = {INT32_MIN, 0};
    std::vector<int32_t> a0(16, 0), a1(16, 0);
    CHECK(extract_scales(zmin, a0.data(), a1.data(), 1, 4, 4, 2, 1, 1, 2, 2,
                         63) == rkvc::Status::Ok);
    CHECK(a0[0] == 63);
    CHECK(extract_scales(nullptr, a0.data(), a1.data(), 1, 4, 4, 2, 1, 1, 2,
                         2, 63) == rkvc::Status::Invalid);
    CHECK(extract_scales(zmin, a0.data(), a1.data(), 0, 4, 4, 2, 1, 1, 2, 2,
                         63) == rkvc::Status::Invalid);
}

TEST_CASE("nc1hwc2 to nchw against naive") {
    const int C = 16, H = 5, W = 10;
    std::vector<uint16_t> src(C * H * W);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = mlvc::pixel::f32_to_f16((i % 77) * 0.25f - 3.0f);
    std::vector<int32_t> got(C * H * W, 0), ref(C * H * W, 0);
    mlvc::pixel::nc1hwc2_to_nchw(src.data(), got.data(), C, H, W);
    // Naive NCHW gather with independent rounding.
    for (int c = 0; c < C; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint16_t h =
                    src[(((c >> 3) * H + y) * W + x) * 8 + (c & 7)];
                ref[(c * H + y) * W + x] = lrintf(
                    mlvc::pixel::f16_to_f32(h));
            }
    CHECK(got == ref);
    std::vector<int32_t> scalar(C * H * W, 0);
    mlvc::pixel::detail::nc1hwc2_to_nchw_scalar(src.data(), scalar.data(), C,
                                               H, W);
    CHECK(scalar == ref);
}

TEST_CASE("nchw int roundtrip through fp16") {
    const int C = 8, H = 4, W = 6;
    std::vector<int32_t> src(C * H * W);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<int32_t>(i % 41) - 20;
    std::vector<uint16_t> mid(C * H * W, 0);
    mlvc::pixel::nchw_to_nc1hwc2_fp16(src.data(), mid.data(), C, H, W);
    std::vector<int32_t> back(C * H * W, 0);
    mlvc::pixel::nc1hwc2_to_nchw(mid.data(), back.data(), C, H, W);
    CHECK(back == src);
}

TEST_CASE("nchw f16 to nc1hwc2 with stride and zero fill") {
    const int C = 10, H = 3, W = 5, C2 = 8, WS = 8;
    std::vector<uint16_t> src(C * H * W);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint16_t>(i & 0xffff);
    int C1 = (C + C2 - 1) / C2;
    std::vector<uint16_t> dst(C1 * H * WS * C2, 0xbeef);
    mlvc::pixel::nchw_f16_to_nc1hwc2(src.data(), dst.data(), C, H, W, C2,
                                     WS);
    for (int c = 0; c < C; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                CHECK(dst[(((c / C2) * H + y) * WS + x) * C2 + (c % C2)] ==
                      src[(c * H + y) * W + x]);
    // Padding (channels beyond C, columns beyond W) is zeroed.
    for (int c1 = 0; c1 < C1; ++c1)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < WS; ++x)
                for (int k = 0; k < C2; ++k) {
                    int c = c1 * C2 + k;
                    if (c >= C || x >= W)
                        CHECK(dst[(((c1) * H + y) * WS + x) * C2 + k] == 0);
                }
}

TEST_CASE("d2s dcr against naive two step") {
    const int C1 = 2, H = 3, W = 4, C2 = 8, bs = 2;
    const int C = C1 * C2, oc = C / (bs * bs), oh = H * bs, ow = W * bs;
    std::vector<uint16_t> src(C * H * W);
    for (size_t i = 0; i < src.size(); ++i)
        src[i] = static_cast<uint16_t>((i * 7) & 0xffff);
    std::vector<uint16_t> got(oc * oh * ow, 0), ref(oc * oh * ow, 0);
    mlvc::pixel::nc1hwc2_d2s_dcr_f16(src.data(), got.data(), C1, H, W, C2,
                                     bs);
    // Naive: NC1HWC2 -> NCHW, then DCR shuffle.
    std::vector<uint16_t> nchw(C * H * W);
    for (int c = 0; c < C; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                nchw[(c * H + y) * W + x] =
                    src[(((c / C2) * H + y) * W + x) * C2 + (c % C2)];
    for (int c = 0; c < oc; ++c)
        for (int h = 0; h < oh; ++h)
            for (int w = 0; w < ow; ++w) {
                int ho = h / bs, wo = w / bs;
                int dy = h % bs, dx = w % bs;
                int src_c = dy * (bs * oc) + dx * oc + c;
                ref[(c * oh + h) * ow + w] =
                    nchw[(src_c * H + ho) * W + wo];
            }
    CHECK(got == ref);
}

TEST_CASE("yuv to nhwc odd height and strides") {
    const int W = 7, H = 5, YS = 8, UVS = 8;
    std::vector<uint8_t> yp(YS * H), uv(UVS * ((H + 1) / 2));
    for (size_t i = 0; i < yp.size(); ++i)
        yp[i] = static_cast<uint8_t>((i * 3) & 0xff);
    for (size_t i = 0; i < uv.size(); ++i)
        uv[i] = static_cast<uint8_t>((i * 5 + 1) & 0xff);
    std::vector<uint16_t> got(W * H * 3, 0);
    // NV12 interleaved: vp points one byte past up in the same buffer.
    mlvc::pixel::yuv_to_nhwc_fp16(yp.data(), YS, uv.data(), uv.data() + 1,
                                  UVS, 1, W, H, got.data());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t yy = yp[y * YS + x];
            uint8_t u = uv[(y / 2) * UVS + (x / 2) * 2];
            uint8_t v = uv[(y / 2) * UVS + (x / 2) * 2 + 1];
            CHECK(got[(y * W + x) * 3 + 0] ==
                  mlvc::pixel::f32_to_f16(yy / 255.0f));
            CHECK(got[(y * W + x) * 3 + 1] ==
                  mlvc::pixel::f32_to_f16(u / 255.0f));
            CHECK(got[(y * W + x) * 3 + 2] ==
                  mlvc::pixel::f32_to_f16(v / 255.0f));
        }
    // I420 split planes.
    std::vector<uint8_t> pu(UVS * ((H + 1) / 2), 0), pv(UVS * ((H + 1) / 2), 0);
    for (size_t i = 0; i < pu.size(); ++i) {
        pu[i] = static_cast<uint8_t>((i * 7) & 0xff);
        pv[i] = static_cast<uint8_t>((i * 11 + 3) & 0xff);
    }
    std::vector<uint16_t> got2(W * H * 3, 0);
    mlvc::pixel::yuv_to_nhwc_fp16(yp.data(), YS, pu.data(), pv.data(), UVS,
                                  0, W, H, got2.data());
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            CHECK(got2[(y * W + x) * 3 + 1] ==
                  mlvc::pixel::f32_to_f16(pu[(y / 2) * UVS + x / 2] /
                                          255.0f));
            CHECK(got2[(y * W + x) * 3 + 2] ==
                  mlvc::pixel::f32_to_f16(pv[(y / 2) * UVS + x / 2] /
                                          255.0f));
        }
}

TEST_CASE("nchw yuv to nv12 saturation and sampling") {
    const int W = 10, H = 6;
    std::vector<uint16_t> src(3 * W * H);
    for (int i = 0; i < W * H; ++i) {
        float f = (i % 300) / 255.0f - 0.1f;  // dips below 0, peaks above 1
        src[i] = mlvc::pixel::f32_to_f16(f);
        src[W * H + i] = mlvc::pixel::f32_to_f16(0.5f);
        src[2 * W * H + i] = mlvc::pixel::f32_to_f16(1.5f);
    }
    std::vector<uint8_t> yp(W * H, 0), uv(W * H / 2, 0);
    mlvc::pixel::nchw_yuv_fp16_to_nv12_planes(src.data(), W, H, yp.data(), W,
                                              uv.data(), W);
    auto sat = [](float f) -> uint8_t {
        int v = lrintf(f * 255.0f);
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    for (int i = 0; i < W * H; ++i)
        CHECK(yp[i] == sat(mlvc::pixel::f16_to_f32(src[i])));
    for (int y = 0; y < H / 2; ++y)
        for (int x = 0; x < W / 2; ++x) {
            float u = mlvc::pixel::f16_to_f32(
                src[W * H + (y * 2) * W + x * 2]);
            float v = mlvc::pixel::f16_to_f32(
                src[2 * W * H + (y * 2) * W + x * 2]);
            CHECK(uv[y * W + x * 2] == sat(u));
            CHECK(uv[y * W + x * 2 + 1] == sat(v));
        }
    std::vector<uint8_t> yp2(W * H, 0), uv2(W * H / 2, 0);
    mlvc::pixel::detail::nchw_yuv_fp16_to_nv12_planes_scalar(
        src.data(), W, H, yp2.data(), W, uv2.data(), W);
    CHECK(yp2 == yp);
    CHECK(uv2 == uv);
}

TEST_CASE("nc1hwc2 to nv12 matches nchw path") {
    const int W = 8, H = 4, C2 = 8;
    std::vector<uint16_t> nchw(3 * W * H);
    for (size_t i = 0; i < nchw.size(); ++i)
        nchw[i] = mlvc::pixel::f32_to_f16(((i * 13) % 300) / 255.0f);
    std::vector<uint16_t> packed(3 * W * C2, 0);
    for (int c = 0; c < 3; ++c)
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                packed[(y * W + x) * C2 + c] = nchw[(c * H + y) * W + x];
    std::vector<uint8_t> a_yp(W * H, 0), a_uv(W * H / 2, 0);
    std::vector<uint8_t> b_yp(W * H, 0), b_uv(W * H / 2, 0);
    mlvc::pixel::nchw_yuv_fp16_to_nv12_planes(nchw.data(), W, H, a_yp.data(),
                                              W, a_uv.data(), W);
    mlvc::pixel::nc1hwc2_fp16_to_nv12_planes(packed.data(), W, H, C2,
                                             b_yp.data(), W, b_uv.data(),
                                             W);
    CHECK(a_yp == b_yp);
    CHECK(a_uv == b_uv);
}
