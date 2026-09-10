// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstring>
#include <vector>

#include "mlvc/container.hpp"
#include "mlvc/pmf.hpp"
#include "mlvc/qppatch.hpp"
#include "mlvc/qptab.hpp"

namespace {

void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

void put64(std::vector<uint8_t>& v, uint64_t x) {
    put32(v, static_cast<uint32_t>(x));
    put32(v, static_cast<uint32_t>(x >> 32));
}

void put_f64(std::vector<uint8_t>& v, double d) {
    uint64_t bits = 0;
    memcpy(&bits, &d, 8);
    put64(v, bits);
}

}  // namespace

TEST_CASE("container header roundtrip") {
    mlvc::Header h;
    h.width = 640;
    h.height = 368;
    h.qp = 21;
    h.iframe_period = 16;
    h.ltr_start_idx = 4;
    h.ltr_period = 8;
    h.flags = mlvc::kHdrCbr;
    h.target_bitrate_bps = 300000;
    uint8_t raw[mlvc::kHdrSize];
    mlvc::write_header(raw, h);
    auto back = mlvc::parse_header(raw, sizeof(raw));
    CHECK(back);
    if (back) {
        CHECK(back.value().width == 640);
        CHECK(back.value().height == 368);
        CHECK(back.value().qp == 21);
        CHECK(back.value().iframe_period == 16);
        CHECK(back.value().ltr_period == 8);
        CHECK(back.value().flags == mlvc::kHdrCbr);
        CHECK(back.value().target_bitrate_bps == 300000);
        CHECK(back.value().frame_count == 0);
    }
    CHECK(mlvc::parse_header(raw, 10).status() == rkvc::Status::Again);
    raw[5] = 0x03;
    CHECK(mlvc::parse_header(raw, sizeof(raw)).status() ==
          rkvc::Status::Format);
    raw[5] = 0x02;
    memset(raw + 8, 0, 4);  // zero width
    CHECK(mlvc::parse_header(raw, sizeof(raw)).status() ==
          rkvc::Status::Format);
}

TEST_CASE("demuxer streams records and drops") {
    mlvc::Header h;
    h.width = 64;
    h.height = 32;
    uint8_t hdr[mlvc::kHdrSize];
    mlvc::write_header(hdr, h);
    std::vector<uint8_t> stream(hdr, hdr + sizeof(hdr));
    uint8_t rec[mlvc::kRecSize];
    mlvc::write_record(rec, 4, 21, mlvc::kRecKeyframe);
    stream.insert(stream.end(), rec, rec + sizeof(rec));
    stream.insert(stream.end(), {10, 20, 30, 40});
    mlvc::write_record(rec, 0, mlvc::kQindexDropped, 0);
    stream.insert(stream.end(), rec, rec + sizeof(rec));
    mlvc::write_record(rec, 2, 22, mlvc::kRecLtrMark);
    stream.insert(stream.end(), rec, rec + sizeof(rec));
    stream.insert(stream.end(), {50, 60});

    mlvc::Demuxer d;
    // Feed byte by byte: Again until complete, same records out.
    size_t got = 0;
    for (size_t i = 0; i < stream.size(); ++i) {
        CHECK(d.append(&stream[i], 1) == rkvc::Status::Ok);
        for (;;) {
            auto r = d.next();
            if (!r) {
                CHECK(r.status() == rkvc::Status::Again);
                break;
            }
            ++got;
            if (got == 1) {
                CHECK(!r.value().drop);
                CHECK(r.value().q_index == 21);
                CHECK(r.value().flags == mlvc::kRecKeyframe);
                CHECK(r.value().size == 4);
                CHECK(r.value().data[0] == 10);
            } else if (got == 2) {
                CHECK(r.value().drop);
                CHECK(r.value().size == 0);
            } else {
                CHECK(!r.value().drop);
                CHECK(r.value().q_index == 22);
                CHECK(r.value().size == 2);
            }
            d.consume_record();
        }
    }
    CHECK(got == 3);
    CHECK(d.frames_emitted() == 3);
}

TEST_CASE("demuxer rejects corrupt stream") {
    mlvc::Demuxer d;
    uint8_t bad[mlvc::kHdrSize] = {};
    CHECK(d.append(bad, sizeof(bad)) == rkvc::Status::Ok);
    CHECK(d.next().status() == rkvc::Status::Format);
    CHECK(d.next().status() == rkvc::Status::Format);  // sticky
}

TEST_CASE("pmf1 gaussian and bitest") {
    std::vector<uint8_t> blob;
    blob.insert(blob.end(), {'P', 'M', 'F', '1'});
    put32(blob, 2);  // nL
    put32(blob, 3);  // nO
    put32(blob, 4);  // nT
    put32(blob, 5);
    put32(blob, 6);  // lengths
    put32(blob, 7);
    put32(blob, 8);
    put32(blob, 9);  // offsets
    for (uint32_t i = 0; i < 4; ++i)
        put32(blob, 100 + i);  // table
    put32(blob, 1);            // gaussian tag
    put_f64(blob, 0.5);
    put_f64(blob, 2.5);
    put32(blob, 8);   // levels
    put32(blob, 16);  // index space
    auto p = mlvc::load_pmf(blob.data(), blob.size());
    CHECK(p);
    if (p) {
        CHECK(p.value().lengths.size() == 2);
        CHECK(p.value().offsets.size() == 3);
        CHECK(p.value().table.size() == 4);
        CHECK(p.value().table[0] == 100);
        CHECK(p.value().gaussian);
        CHECK(p.value().scale_min == 0.5);
        CHECK(p.value().scale_max == 2.5);
        CHECK(p.value().scale_levels == 8);
    }
    blob[0] = 'X';
    CHECK(mlvc::load_pmf(blob.data(), blob.size()).status() ==
          rkvc::Status::Format);
}

TEST_CASE("qpt1 tables with clamp") {
    std::vector<uint8_t> blob;
    blob.insert(blob.end(), {'Q', 'P', 'T', '1'});
    put32(blob, 1);  // one table
    const char* name = "q_encoder_row";
    put32(blob, 13);
    blob.insert(blob.end(), name, name + 13);
    put32(blob, 2);  // rows
    put32(blob, 3);  // cols
    for (uint16_t i = 0; i < 6; ++i) {
        blob.push_back(static_cast<uint8_t>(i));
        blob.push_back(0);
    }
    auto q = mlvc::load_qptab(blob.data(), blob.size());
    CHECK(q);
    if (q) {
        CHECK(q.value().tables.size() == 1);
        const mlvc::QpTable* t = q.value().find("q_encoder_row");
        CHECK(t != nullptr);
        if (t) {
            CHECK(t->rows == 2);
            CHECK(t->cols == 3);
            CHECK(mlvc::qptab_row(*t, 0)[2] == 2);
            CHECK(mlvc::qptab_row(*t, 1)[0] == 3);
            CHECK(mlvc::qptab_row(*t, 99)[0] == 3);  // clamped
        }
        CHECK(q.value().find("nope") == nullptr);
    }
    blob.push_back(0);  // trailing byte
    CHECK(mlvc::load_qptab(blob.data(), blob.size()).status() ==
          rkvc::Status::Format);
}

TEST_CASE("crc32 isotest") {
    const char* v = "123456789";
    CHECK(mlvc::crc32(reinterpret_cast<const uint8_t*>(v), 9) == 0xCBF43926u);
    CHECK(mlvc::crc32(nullptr, 0) == 0);
}

TEST_CASE("qpp1 apply with crc") {
    std::vector<uint8_t> base = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<uint8_t> patch;
    patch.insert(patch.end(), {'Q', 'P', 'P', '1'});
    put32(patch, 1);  // version
    put64(patch, base.size());
    put32(patch, 21);  // qp
    put32(patch, 1);   // one range
    put32(patch, 0);   // flags
    put32(patch, mlvc::crc32(base.data(), base.size()));
    std::vector<uint8_t> payload = {9, 9};
    put32(patch, mlvc::crc32(payload.data(), payload.size()));
    put32(patch, 0);  // coalesce gap note
    put32(patch, 0);  // reserved
    put32(patch, 0);  // reserved
    put32(patch, 2);  // off
    put32(patch, 2);  // len
    patch.insert(patch.end(), payload.begin(), payload.end());
    CHECK(mlvc::apply_qppatch(base.data(), base.size(), patch.data(),
                              patch.size(), 21));
    CHECK(base[2] == 9);
    CHECK(base[3] == 9);
    CHECK(base[0] == 1);
    CHECK(mlvc::apply_qppatch(base.data(), base.size(), patch.data(),
                              patch.size(), 22)
              .status() == rkvc::Status::Format);  // qp mismatch + crc moved
    patch.back() ^= 0xff;                          // corrupt payload
    CHECK(mlvc::apply_qppatch(base.data(), base.size(), patch.data(),
                              patch.size(), -1)
              .status() == rkvc::Status::Format);
}
