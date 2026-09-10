// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "rkvc/result.hpp"
#include "rkvc/status.hpp"

namespace pspack {

// GB28181-2022 video-only MPEG-2 program stream, H.264/H.265 over PS.
// Mux emits one access unit per call, fragmented into <=60KB PES packets
// with explicit lengths (first PES carries PTS and data_alignment=1).
// Demux pairs with this framing only; foreign PS needs an ES parser.
enum class VideoCodec : uint8_t { Avc = 0, Hevc = 1 };

struct MuxOptions {
    uint32_t mux_rate_50bs = 25000;  // program_mux_rate, 50B/s units.
};

struct Frame {
    std::vector<uint8_t> annexb;
    int64_t pts90 = 0;
    bool keyframe = false;
    VideoCodec codec = VideoCodec::Hevc;
};

constexpr size_t kMaxPayload = 64u << 20;
constexpr uint8_t kVideoStreamId = 0xE0;

// Annex-B NAL scan (3/4-byte start codes).
bool is_keyframe(VideoCodec codec, const uint8_t* data, size_t size) noexcept;

rkvc::Result<std::vector<uint8_t>> mux_frame(VideoCodec codec,
                                             const uint8_t* annexb, size_t size,
                                             int64_t pts90, bool keyframe,
                                             const MuxOptions& opts = MuxOptions{});

// AU assembler: emits AU N when AU N+1 begins (one-AU delay, PTS-correct),
// or on flush() at end of stream. Again = need more bytes.
class Demux {
  public:
    rkvc::Status append(const uint8_t* data, size_t size);
    rkvc::Result<Frame> next();
    rkvc::Result<Frame> flush();

  private:
    rkvc::Result<Frame> emit_au();

    std::vector<uint8_t> buf_;
    size_t cursor_ = 0;
    std::vector<uint8_t> au_;
    int64_t au_pts_ = 0;
    bool au_open_ = false;
    bool have_psm_ = false;
    VideoCodec psm_codec_ = VideoCodec::Hevc;
};

}  // namespace pspack
