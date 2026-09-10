// SPDX-License-Identifier: AGPL-3.0-or-later
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <vector>

#include "pspack/ps.hpp"

namespace {

bool start_at(const std::vector<uint8_t>& ps, size_t off, uint8_t id) {
    return off + 3 < ps.size() && ps[off] == 0 && ps[off + 1] == 0 &&
           ps[off + 2] == 1 && ps[off + 3] == id;
}

// HEVC keyframe AU: VPS + IDR; P AU: single TRAIL NAL.
const uint8_t kHevcKey[] = {0, 0, 0, 1, 0x40, 0xAA, 0, 0, 0, 1, 0x26, 0xBB};
const uint8_t kHevcP[] = {0, 0, 0, 1, 0x02, 0xCC};

}  // namespace

TEST_CASE("keyframe scan") {
    using pspack::is_keyframe;
    using pspack::VideoCodec;
    CHECK(is_keyframe(VideoCodec::Hevc, kHevcKey, sizeof(kHevcKey)));
    CHECK(!is_keyframe(VideoCodec::Hevc, kHevcP, sizeof(kHevcP)));
    const uint8_t avc_key[] = {0, 0, 1, 0x67, 0x42, 0, 0, 1, 0x65, 0x11};
    const uint8_t avc_p[] = {0, 0, 1, 0x41, 0x22};
    CHECK(is_keyframe(VideoCodec::Avc, avc_key, sizeof(avc_key)));
    CHECK(!is_keyframe(VideoCodec::Avc, avc_p, sizeof(avc_p)));
    CHECK(!is_keyframe(VideoCodec::Hevc, nullptr, 0));
    CHECK(!is_keyframe(VideoCodec::Hevc, kHevcP, 0));
}

TEST_CASE("mux packet shape") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    auto key =
        mux_frame(VideoCodec::Hevc, kHevcKey, sizeof(kHevcKey), 9000, true);
    CHECK(key);
    if (key) {
        // pack(14) + system(15) + PSM(20) + video PES: fixed offsets.
        const auto& ps = key.value();
        CHECK(start_at(ps, 0, 0xBA));
        CHECK(start_at(ps, 14, 0xBB));
        CHECK(start_at(ps, 29, 0xBC));
        CHECK(start_at(ps, 49, 0xE0));
        // PSM carries stream_type 0x24 for HEVC.
        CHECK(ps[29 + 12] == 0x24);
    }
    auto p = mux_frame(VideoCodec::Hevc, kHevcP, sizeof(kHevcP), 12000, false);
    CHECK(p);
    if (p) {
        CHECK(start_at(p.value(), 0, 0xBA));
        CHECK(start_at(p.value(), 14, 0xE0));
    }
    auto avc = mux_frame(VideoCodec::Avc, kHevcP, sizeof(kHevcP), 0, true);
    CHECK(avc);
    if (avc) {
        const auto& ps = avc.value();
        bool psm_avc = false;
        for (size_t i = 0; i + 12 < ps.size(); ++i) {
            if (ps[i] == 0 && ps[i + 1] == 0 && ps[i + 2] == 1 &&
                ps[i + 3] == 0xBC)
                psm_avc = ps[i + 12] == 0x1B;
        }
        CHECK(psm_avc);
    }
}

TEST_CASE("mux limits") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    CHECK(mux_frame(VideoCodec::Hevc, nullptr, 10, 0, true).status() ==
          rkvc::Status::Invalid);
    CHECK(mux_frame(VideoCodec::Hevc, kHevcP, 0, 0, true).status() ==
          rkvc::Status::Invalid);
    CHECK(mux_frame(VideoCodec::Hevc, kHevcP, pspack::kMaxPayload + 1, 0, true)
              .status() == rkvc::Status::Format);
}

TEST_CASE("roundtrip three frames") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    const int64_t pts[] = {0, 3000, 6000};
    const uint8_t* au[] = {kHevcKey, kHevcP, kHevcP};
    const size_t au_n[] = {sizeof(kHevcKey), sizeof(kHevcP), sizeof(kHevcP)};
    const bool key[] = {true, false, false};
    std::vector<uint8_t> stream;
    for (int i = 0; i < 3; ++i) {
        auto r = mux_frame(VideoCodec::Hevc, au[i], au_n[i], pts[i], key[i]);
        CHECK(r);
        if (r)
            stream.insert(stream.end(), r.value().begin(), r.value().end());
    }
    pspack::Demux demux;
    CHECK(demux.append(stream.data(), stream.size()) == rkvc::Status::Ok);
    // AU N is emitted when AU N+1 begins; the tail drains via flush().
    for (int i = 0; i < 2; ++i) {
        auto f = demux.next();
        CHECK(f);
        if (!f)
            continue;
        CHECK(f.value().annexb == std::vector<uint8_t>(au[i], au[i] + au_n[i]));
        CHECK(f.value().pts90 == pts[i]);
        CHECK(f.value().keyframe == key[i]);
        CHECK(f.value().codec == VideoCodec::Hevc);
    }
    CHECK(demux.next().status() == rkvc::Status::Again);
    auto tail = demux.flush();
    CHECK(tail);
    if (tail) {
        CHECK(tail.value().annexb ==
              std::vector<uint8_t>(au[2], au[2] + au_n[2]));
        CHECK(tail.value().pts90 == pts[2]);
        CHECK(tail.value().keyframe == key[2]);
    }
    CHECK(demux.flush().status() == rkvc::Status::Eof);
    CHECK(pspack::Demux{}.flush().status() == rkvc::Status::Eof);
}

TEST_CASE("chunked feed and resync") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    auto r =
        mux_frame(VideoCodec::Hevc, kHevcKey, sizeof(kHevcKey), 3000, true);
    CHECK(r);
    auto r2 = mux_frame(VideoCodec::Hevc, kHevcP, sizeof(kHevcP), 6000, false);
    CHECK(r2);
    if (!r || !r2)
        return;
    pspack::Demux demux;
    // Leading garbage is skipped, split appends still reassemble.
    const uint8_t junk[] = {0xFF, 0x00, 0x11};
    CHECK(demux.append(junk, sizeof(junk)) == rkvc::Status::Ok);
    const auto& ps = r.value();
    for (size_t off = 0; off < ps.size();) {
        size_t n = off + 7 <= ps.size() ? 7 : ps.size() - off;
        CHECK(demux.append(ps.data() + off, n) == rkvc::Status::Ok);
        off += n;
        CHECK(demux.next().status() == rkvc::Status::Again);
    }
    CHECK(demux.append(r2.value().data(), r2.value().size()) ==
          rkvc::Status::Ok);
    auto got = demux.next();
    CHECK(got);
    if (got) {
        CHECK(got.value().pts90 == 3000);
        CHECK(got.value().keyframe);
        CHECK(got.value().annexb ==
              std::vector<uint8_t>(kHevcKey, kHevcKey + sizeof(kHevcKey)));
    }
    auto tail = demux.flush();
    CHECK(tail);
    if (tail)
        CHECK(tail.value().pts90 == 6000);
    CHECK(demux.append(nullptr, 4) == rkvc::Status::Format);
}

TEST_CASE("large frame splits across PES") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    std::vector<uint8_t> big = {0, 0, 0, 1, 0x26};
    big.insert(big.end(), 70000, 0xAB);
    auto r = mux_frame(VideoCodec::Hevc, big.data(), big.size(), 9000, true);
    CHECK(r);
    if (!r)
        return;
    // Two packs: the AU exceeds the 60KB PES chunk.
    CHECK(start_at(r.value(), 0, 0xBA));
    auto tail =
        mux_frame(VideoCodec::Hevc, kHevcP, sizeof(kHevcP), 12000, false);
    CHECK(tail);
    if (!tail)
        return;
    pspack::Demux demux;
    CHECK(demux.append(r.value().data(), r.value().size()) == rkvc::Status::Ok);
    CHECK(demux.append(tail.value().data(), tail.value().size()) ==
          rkvc::Status::Ok);
    auto f = demux.next();
    CHECK(f);
    if (f) {
        CHECK(f.value().annexb == big);
        CHECK(f.value().pts90 == 9000);
        CHECK(f.value().keyframe);
    }
    auto last = demux.flush();
    CHECK(last);
    if (last)
        CHECK(last.value().pts90 == 12000);
}

TEST_CASE("pts wraps at 33 bits") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    int64_t pts = (1LL << 33) + 12345;
    auto r = mux_frame(VideoCodec::Hevc, kHevcP, sizeof(kHevcP), pts, false);
    CHECK(r);
    if (!r)
        return;
    auto fin =
        mux_frame(VideoCodec::Hevc, kHevcP, sizeof(kHevcP), pts + 3000, false);
    CHECK(fin);
    if (!fin)
        return;
    pspack::Demux demux;
    CHECK(demux.append(r.value().data(), r.value().size()) == rkvc::Status::Ok);
    CHECK(demux.append(fin.value().data(), fin.value().size()) ==
          rkvc::Status::Ok);
    auto f = demux.next();
    CHECK(f);
    if (f)
        CHECK(f.value().pts90 == 12345);
}

namespace {

// 5-byte PTS field, marker bits interleaved (ISO/IEC 13818-1).
void put_pts(std::vector<uint8_t>& o, int64_t pts) {
    const uint64_t p = static_cast<uint64_t>(pts) & 0x1FFFFFFFFULL;
    o.push_back(static_cast<uint8_t>(0x20 | (((p >> 30) & 7) << 1) | 1));
    o.push_back(static_cast<uint8_t>(p >> 22));
    o.push_back(static_cast<uint8_t>(((p >> 14) & 0xFE) | 1));
    o.push_back(static_cast<uint8_t>(p >> 7));
    o.push_back(static_cast<uint8_t>(((p << 1) & 0xFE) | 1));
}

// One video PES with data_alignment (AU start) and an explicit PTS; with_dts
// switches the PTS_DTS_flags to '11' and appends a throwaway DTS.
void put_video_pes(std::vector<uint8_t>& o, int64_t pts, bool with_dts,
                   const std::vector<uint8_t>& payload) {
    const size_t hlen = with_dts ? 10 : 5;
    o.insert(o.end(), {0, 0, 1, 0xE0});
    const size_t len = 3 + hlen + payload.size();
    o.push_back(static_cast<uint8_t>(len >> 8));
    o.push_back(static_cast<uint8_t>(len));
    o.push_back(0x84);
    o.push_back(with_dts ? 0xC0 : 0x80);
    o.push_back(static_cast<uint8_t>(hlen));
    put_pts(o, pts);
    if (with_dts)
        put_pts(o, pts);
    o.insert(o.end(), payload.begin(), payload.end());
}

}  // namespace

TEST_CASE("PSM decides the codec when the NALs are ambiguous") {
    using pspack::mux_frame;
    using pspack::VideoCodec;
    // 0x02 matches neither AVC (5/7) nor HEVC (19/20/32) NAL types, so the
    // AU alone cannot be sniffed: the PSM in the keyframe pack must win.
    const uint8_t ambiguous[] = {0, 0, 1, 0x02, 0x33};
    auto first =
        mux_frame(VideoCodec::Avc, ambiguous, sizeof(ambiguous), 9000, true);
    auto second =
        mux_frame(VideoCodec::Avc, kHevcP, sizeof(kHevcP), 12000, false);
    CHECK(first);
    CHECK(second);
    if (!first || !second)
        return;
    pspack::Demux demux;
    CHECK(demux.append(first.value().data(), first.value().size()) ==
          rkvc::Status::Ok);
    CHECK(demux.append(second.value().data(), second.value().size()) ==
          rkvc::Status::Ok);
    auto f = demux.next();
    CHECK(f);
    if (f) {
        CHECK(f.value().codec == VideoCodec::Avc);
        CHECK(f.value().annexb ==
              std::vector<uint8_t>(ambiguous, ambiguous + sizeof(ambiguous)));
        CHECK(f.value().pts90 == 9000);
    }
}

TEST_CASE("PTS+DTS flagged PES keeps the presentation time") {
    const std::vector<uint8_t> au0 = {0, 0, 1, 0x02, 0xAA};
    const std::vector<uint8_t> au1 = {0, 0, 1, 0x02, 0xBB};
    std::vector<uint8_t> stream;
    put_video_pes(stream, 123456, true, au0);
    put_video_pes(stream, 126456, false, au1);
    pspack::Demux demux;
    CHECK(demux.append(stream.data(), stream.size()) == rkvc::Status::Ok);
    auto f = demux.next();
    CHECK(f);
    if (f) {
        CHECK(f.value().pts90 == 123456);
        CHECK(f.value().annexb == au0);
    }
    auto tail = demux.flush();
    CHECK(tail);
    if (tail)
        CHECK(tail.value().pts90 == 126456);
}
