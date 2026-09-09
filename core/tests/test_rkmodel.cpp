// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "rkvc/rkmodel.hpp"
#include "rkvc/sha256.hpp"

static std::string hex_of(const uint8_t* p, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s += digits[p[i] >> 4];
        s += digits[p[i] & 0xf];
    }
    return s;
}

TEST_CASE("sha256 known answers") {
    using namespace rkvc;
    // Vectors cross-checked against CPython hashlib (all 'a' repeats).
    const char* Ns[] = {"", "aaa"};
    const char* want[] = {
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "9834876dcfb05cb167a5c24953eba58c4ac89b1adf57f28f2f9d09af107ee8f0",
    };
    for (int i = 0; i < 2; ++i) {
        std::string in(Ns[i]);
        auto h = sha256(reinterpret_cast<const uint8_t*>(in.data()),
                        in.size());
        CHECK(hex_of(h.data(), h.size()) == want[i]);
    }
    for (int n : {55, 56, 64, 1000}) {
        std::string in(static_cast<size_t>(n), 'a');
        auto h = sha256(reinterpret_cast<const uint8_t*>(in.data()),
                        in.size());
        const char* w = nullptr;
        if (n == 55)
            w = "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f"
                "734318";
        else if (n == 56)
            w = "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686e"
                "c6738a";
        else if (n == 64)
            w = "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df15"
                "4668eb";
        else
            w = "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b"
                "9737ea3";
        INFO("n=", n);
        CHECK(hex_of(h.data(), h.size()) == std::string(w));
    }
}

TEST_CASE("rkmodel roundtrip") {
    using namespace rkvc;
    Model m;
    m.meta.id = "mlvc-bench-qp21";
    m.meta.family = "mlvc";
    m.meta.role = "encoder";
    m.meta.target = "rk3576";
    ModelPayload a;
    a.kind = "rknn";
    a.data = {1, 2, 3, 4, 5};
    ModelPayload b;
    b.kind = "pmf-gaussian";
    b.flags = 1;
    b.data = {9, 8, 7};
    m.payloads = {a, b};
    auto packed = pack_model(m);
    CHECK(packed);
    auto back = unpack_model(packed.value().data(), packed.value().size());
    CHECK(back);
    CHECK(back.value().meta.id == "mlvc-bench-qp21");
    CHECK(back.value().meta.target == "rk3576");
    CHECK(back.value().find("rknn") != nullptr);
    CHECK(back.value().find("rknn")->data == a.data);
    CHECK(back.value().find("pmf-gaussian")->flags == 1);
    CHECK(back.value().find("missing") == nullptr);
}

TEST_CASE("rkmodel rejects old magic and corruption") {
    using namespace rkvc;
    Model m;
    m.meta.id = "x";
    m.meta.family = "mlvc";
    m.meta.role = "encoder";
    ModelPayload a;
    a.kind = "rknn";
    a.data = {1, 2, 3};
    m.payloads = {a};
    auto packed = pack_model(m);
    CHECK(packed);
    // Old "RKMF" magic rejected.
    packed.value()[0] = 'R';
    packed.value()[1] = 'K';
    packed.value()[2] = 'M';
    packed.value()[3] = 'F';
    Diag d;
    auto bad = unpack_model(packed.value().data(), packed.value().size(), &d);
    CHECK(!bad);
    CHECK(bad.status() == Status::Format);
    // Flip a payload byte: integrity error.
    auto packed2 = pack_model(m);
    CHECK(packed2);
    packed2.value().back() ^= 0xff;
    auto corrupt =
        unpack_model(packed2.value().data(), packed2.value().size(), &d);
    CHECK(!corrupt);
    CHECK(corrupt.status() == Status::Integrity);
    // Duplicate kind rejected at pack.
    Model dup = m;
    dup.payloads.push_back(a);
    CHECK(!pack_model(dup));
}
