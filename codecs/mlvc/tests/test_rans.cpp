// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

#include "mlvc/rans.hpp"

namespace {

const int32_t kLengths[] = {5, 4};
const int32_t kOffsets[] = {3, 1};
const int32_t kTable[] = {64, 64, 64, 32, 32, 128, 64, 32, 32};
const int32_t kPat[] = {-5, 0, 1, 2, 3, 4, 100, -100};
// Goldens produced by the C implementation (same PMF/pattern).
const char* kByteHex =
    "fd3f0020bcf8f4d257bcfdc0f2e7c6f7f1fdfcc8fd19dfc5fdbc12f2e7c6f7f1fdfcd"
    "2d2e7f157fccdfcd252e7f1d7f1fcc8f9f419e7f3e01212f731";
const char* k64Hex =
    "fd3f000020000000a0774fde57dc0efdf2cec7f7713bf4ee9d8fefc5edd43d79e7e3"
    "7b718dba27ef677c2f7e0dde3d79e7e37bf1e0dd933719b743e792bc7b31";

std::vector<uint8_t> unhex(const char* h) {
    std::vector<uint8_t> out;
    auto nib = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9')
            return static_cast<uint8_t>(c - '0');
        return static_cast<uint8_t>(c - 'a' + 10);
    };
    for (size_t i = 0; h[i] && h[i + 1]; i += 2)
        out.push_back(static_cast<uint8_t>((nib(h[i]) << 4) | nib(h[i + 1])));
    return out;
}

std::string hex_of(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 0xf];
    }
    return s;
}

void fill_pattern(std::vector<int32_t>& idx, std::vector<int32_t>& val) {
    idx.resize(64);
    val.resize(64);
    for (int i = 0; i < 64; ++i) {
        idx[i] = (i % 7 == 6) ? -1 : (i % 2);
        val[i] = kPat[i % 8];
    }
}

mlvc::RansCoder make_coder(mlvc::RansVariant v) {
    auto r = mlvc::RansCoder::create(v, std::span<const int32_t>(kLengths, 2),
                                     std::span<const int32_t>(kOffsets, 2),
                                     std::span<const int32_t>(kTable, 9), 8, 2);
    CHECK(r);
    if (!r)
        return mlvc::RansCoder{};
    return std::move(r.value());
}

}  // namespace

TEST_CASE("rans decodes C goldens") {
    for (auto [v, hx] : {std::pair{mlvc::RansVariant::Byte, kByteHex},
                         std::pair{mlvc::RansVariant::Rans64, k64Hex}}) {
        mlvc::RansCoder coder = make_coder(v);
        std::vector<int32_t> idx, val;
        fill_pattern(idx, val);
        std::vector<uint8_t> bytes = unhex(hx);
        std::vector<int32_t> back(64, 0x12345678);
        CHECK(mlvc::rans_decode(coder, back, idx, bytes) == rkvc::Status::Ok);
        for (int i = 0; i < 64; ++i) {
            int32_t want = (idx[i] < 0) ? 0 : val[i];
            CHECK(back[i] == want);
        }
    }
}

TEST_CASE("rans encodes byte-identical goldens") {
    for (auto [v, hx] : {std::pair{mlvc::RansVariant::Byte, kByteHex},
                         std::pair{mlvc::RansVariant::Rans64, k64Hex}}) {
        mlvc::RansCoder coder = make_coder(v);
        std::vector<int32_t> idx, val;
        fill_pattern(idx, val);
        auto enc = mlvc::rans_encode(coder, idx, val);
        CHECK(enc);
        if (enc)
            CHECK(hex_of(enc.value()) == hx);
    }
}

TEST_CASE("rans streaming roundtrip with two coders") {
    mlvc::RansCoder coder = make_coder(mlvc::RansVariant::Byte);
    std::vector<int32_t> idx(256), val(256);
    uint32_t rng = 0x12345u;
    for (int i = 0; i < 256; ++i) {
        rng = rng * 1103515245u + 12345u;
        idx[i] = (rng >> 16) % 3 - 1;  // -1, 0, 1
        val[i] = static_cast<int32_t>((rng >> 8) % 211) - 105;
    }
    mlvc::RansEncoder enc(mlvc::RansVariant::Byte);
    // Two coders / two calls share one stream, like gaussian + bitest.
    CHECK(enc.encode(coder, std::span<const int32_t>(idx.data(), 128),
                     std::span<const int32_t>(val.data(), 128)) ==
          rkvc::Status::Ok);
    CHECK(enc.encode(coder, std::span<const int32_t>(idx.data() + 128, 128),
                     std::span<const int32_t>(val.data() + 128, 128)) ==
          rkvc::Status::Ok);
    const uint8_t* out = nullptr;
    size_t size = 0;
    CHECK(enc.flush(&out, &size) == rkvc::Status::Ok);
    CHECK(size > 0);
    mlvc::RansDecoder dec(mlvc::RansVariant::Byte);
    CHECK(dec.open(std::span<const uint8_t>(out, size)) == rkvc::Status::Ok);
    std::vector<int32_t> back(256, 0);
    // LIFO order: the last encoded group decodes first.
    CHECK(dec.decode(coder, std::span<int32_t>(back.data() + 128, 128),
                     std::span<const int32_t>(idx.data() + 128, 128)) ==
          rkvc::Status::Ok);
    CHECK(dec.decode(coder, std::span<int32_t>(back.data(), 128),
                     std::span<const int32_t>(idx.data(), 128)) ==
          rkvc::Status::Ok);
    CHECK(dec.check_eof());
    for (int i = 0; i < 256; ++i) {
        int32_t want = (idx[i] < 0) ? 0 : val[i];
        CHECK(back[i] == want);
    }
}

TEST_CASE("rans rejects bad input") {
    mlvc::RansCoder coder = make_coder(mlvc::RansVariant::Byte);
    std::vector<uint8_t> bytes = unhex(kByteHex);
    std::vector<int32_t> idx(64, 0), back(64, 0);
    // Truncated stream.
    CHECK(mlvc::rans_decode(
              coder, back, idx,
              std::span<const uint8_t>(bytes.data(), bytes.size() / 2)) !=
          rkvc::Status::Ok);
    // Corrupt stream.
    bytes[10] ^= 0xff;
    bytes[20] ^= 0x01;
    rkvc::Status st = mlvc::rans_decode(coder, back, idx, bytes);
    CHECK(st != rkvc::Status::Ok);
    // Bad coder params.
    CHECK(mlvc::RansCoder::create(mlvc::RansVariant::Byte,
                                  std::span<const int32_t>(kLengths, 2),
                                  std::span<const int32_t>(kOffsets, 1),
                                  std::span<const int32_t>(kTable, 9), 8, 2)
              .status() == rkvc::Status::Invalid);
    const int32_t bad_table[] = {64, 64, 64, 32, 33, 128, 64, 32, 32};
    CHECK(mlvc::RansCoder::create(mlvc::RansVariant::Byte,
                                  std::span<const int32_t>(kLengths, 2),
                                  std::span<const int32_t>(kOffsets, 2),
                                  std::span<const int32_t>(bad_table, 9), 8, 2)
              .status() == rkvc::Status::Format);
}
