// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "rkvc/frame.hpp"
#include "rkvc/spec.hpp"

TEST_CASE("unify wildcards") {
    using namespace rkvc;
    Spec any;
    Spec nv12;
    nv12.width = 640;
    nv12.height = 480;
    nv12.fmt = PixelFormat::Nv12;
    auto r = unify(any, nv12);
    CHECK(r);
    CHECK(r.value().fmt == PixelFormat::Nv12);
    CHECK(r.value().width == 640);
    auto r2 = unify(nv12, any);
    CHECK(r2);
    CHECK(r2.value().height == 480);
}

TEST_CASE("unify dimension merge and conflict") {
    using namespace rkvc;
    Spec a, b;
    a.fmt = PixelFormat::Nv12;
    b.fmt = PixelFormat::Nv12;
    a.width = 640;
    Diag d;
    auto ok = unify(a, b, &d);
    CHECK(ok);
    CHECK(ok.value().width == 640);
    b.width = 320;
    auto bad = unify(a, b, &d);
    CHECK(!bad);
    CHECK(bad.status() == Status::Negotiate);
    Spec c = a, e = a;
    e.fmt = PixelFormat::Yuv420P;
    CHECK(!unify(c, e));
    e = a;
    e.domain = MemDomain::Dmabuf;
    CHECK(!unify(c, e));
}

TEST_CASE("spec sizes") {
    using namespace rkvc;
    Spec s;
    s.width = 64;
    s.height = 32;
    s.fmt = PixelFormat::Nv12;
    CHECK(eff_stride(s) == 64);
    CHECK(eff_ver_stride(s) == 32);
    CHECK(min_size(s) == 64u * 32u * 3 / 2);
    s.stride = 128;
    CHECK(min_size(s) == 128u * 32u * 3 / 2);
    Spec bs;
    bs.fmt = PixelFormat::Bitstream;
    CHECK(min_size(bs) == 0);
}

TEST_CASE("borrow host validates payload") {
    using namespace rkvc;
    Spec s;
    s.width = 64;
    s.height = 32;
    s.fmt = PixelFormat::Nv12;
    std::vector<uint8_t> buf(min_size(s));
    Diag d;
    CHECK(!Frame::borrow_host(s, nullptr, 0, {}, &d));
    CHECK(!Frame::borrow_host(s, buf.data(), 10, {}, &d));
    auto ok = Frame::borrow_host(s, buf.data(), buf.size());
    CHECK(ok);
    CHECK(ok.value()->data() == buf.data());
    CHECK(ok.value()->fd() == -1);
    CHECK(ok.value()->pts() == kTsUnknown);
}

TEST_CASE("release hook fires on last release") {
    using namespace rkvc;
    Spec s;
    s.width = 16;
    s.height = 16;
    s.fmt = PixelFormat::Nv12;
    std::vector<uint8_t> buf(min_size(s));
    int fired = 0;
    FrameHooks hooks{[](void* ctx) noexcept {
                         *static_cast<int*>(ctx) += 1;
                     },
                     &fired};
    auto ok = Frame::borrow_host(s, buf.data(), buf.size(), hooks);
    CHECK(ok);
    {
        auto alias = ok.value();
        CHECK(fired == 0);
    }
    CHECK(fired == 0);
    ok.value().reset();
    CHECK(fired == 1);
}

TEST_CASE("borrow dmabuf linear only") {
    using namespace rkvc;
    Spec s;
    s.width = 64;
    s.height = 32;
    s.fmt = PixelFormat::Nv12;
    s.domain = MemDomain::Dmabuf;
    CHECK(!Frame::borrow_dmabuf(s, -1, 0));
    s.modifier = 1;
    CHECK(!Frame::borrow_dmabuf(s, 7, 0));
    s.modifier = 0;
    auto ok = Frame::borrow_dmabuf(s, 7, 1024);
    CHECK(ok);
    CHECK(ok.value()->fd() == 7);
    CHECK(ok.value()->data() == nullptr);
}

TEST_CASE("roi validation") {
    using namespace rkvc;
    Spec s;
    s.width = 64;
    s.height = 64;
    s.fmt = PixelFormat::Nv12;
    std::vector<uint8_t> buf(min_size(s));
    auto f = Frame::borrow_host(s, buf.data(), buf.size());
    CHECK(f);
    RoiRegion r;
    r.width = 200;
    r.height = 16;
    CHECK(!f.value()->set_roi(&r, 1));
    r.width = 32;
    r.qp_delta = 60;
    CHECK(!f.value()->set_roi(&r, 1));
    r.qp_delta = -8;
    CHECK(f.value()->set_roi(&r, 1));
    CHECK(f.value()->roi_count() == 1);
}
