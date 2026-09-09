// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace mlvc {

// .mlvc stream container. Wire format is byte-identical to the C
// implementation (64B header + 16B records, magic "MLVC1", format byte
// 0x02): the format was already minimal, so the redesign keeps the bytes
// and drops nothing on the floor for board interop. The 0x02 byte is the
// one sanctioned exception to the version-1 policy: unifying it would fork
// the live wire format shared with the old C tree for no functional gain.
inline constexpr size_t kHdrSize = 64;
inline constexpr size_t kRecSize = 16;
inline constexpr uint32_t kMaxFrameBytes = 64u << 20;
inline constexpr int32_t kQindexDropped = -1;
inline constexpr uint32_t kRecKeyframe = 0x1u;
inline constexpr uint32_t kRecLtrMark = 0x2u;
inline constexpr uint32_t kRecLtrRecovery = 0x4u;
inline constexpr uint32_t kHdrProactiveLtr = 0x1u;
inline constexpr uint32_t kHdrCbr = 0x2u;

struct Header {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    uint32_t qp = 0;
    uint32_t frame_count = 0;  // always 0; readers use EOF
    uint32_t iframe_period = 0;
    uint32_t ltr_start_idx = 0;
    uint32_t ltr_period = 0;
    uint32_t flags = 0;
    uint32_t target_bitrate_bps = 0;
};

void write_header(uint8_t out[kHdrSize], const Header& hdr);
// Ok: parsed. Again: fewer than 64 bytes. Format: magic/format/dims bad.
rkvc::Result<Header> parse_header(const uint8_t* data, size_t size);
void write_record(uint8_t out[kRecSize], uint32_t payload_size,
                  int32_t q_index, uint32_t flags);

// Streaming demuxer: append bytes, pull records, consume each record.
// next(): Ok(view into internal buffer) / Again (need data) / Format
// (sticky parse error).
class Demuxer {
public:
    struct View {
        bool drop = false;  // dropped frame marker, no payload
        int32_t q_index = 0;
        uint32_t flags = 0;
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    rkvc::Status append(const uint8_t* data, size_t size);
    rkvc::Result<View> next();
    void consume_record() noexcept;
    bool has_header() const noexcept { return have_header_; }
    const Header& header() const noexcept { return header_; }
    uint64_t frames_emitted() const noexcept { return frames_emitted_; }

private:
    std::vector<uint8_t> buf_;
    Header header_;
    bool have_header_ = false;
    bool parse_error_ = false;
    uint64_t frames_emitted_ = 0;
    size_t last_record_ = 0;  // bytes to drop on consume_record()
};

}  // namespace mlvc
