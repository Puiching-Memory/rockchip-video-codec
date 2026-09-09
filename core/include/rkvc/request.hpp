// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"
#include "rkvc/spec.hpp"

namespace rkvc {

enum class Operation : uint32_t {
    Transcode = 0,
    Decode,
    Encode,
    Upscale,
};

enum class Codec : uint32_t {
    Auto = 0,
    H264,
    Hevc,
    Av1,
    Mlvc,
};

enum class Policy : uint32_t {
    Realtime = 0,
    Balanced,
    Quality,
    Offline,
};

enum class EndpointKind : uint32_t {
    File = 0,
    FrameSink,
    Stream,
};

struct Quality {
    int32_t bitrate_bps = 0;  // <=0: automatic
    int32_t qp = -1;          // <0: automatic
    uint32_t gop_size = 0;    // 0: backend default
    uint32_t ltr_period = 0;  // 0: automatic
    uint32_t ltr_start_idx = 0;
};

struct Endpoint {
    EndpointKind kind = EndpointKind::FrameSink;
    std::string uri;  // File/Stream address
};

// One media request: intent only, the planner picks the path.
struct Request {
    Operation operation = Operation::Encode;
    Codec codec = Codec::Auto;
    Policy policy = Policy::Balanced;
    Quality quality;
    Endpoint input;
    Endpoint output;
    Spec input_spec;   // FrameSink caller-side format (UNKNOWN = decide later)
    Spec output_spec;  // FrameSink caller-side format (UNKNOWN = decide later)
    uint32_t width = 0;   // 0: follow source
    uint32_t height = 0;  // 0: follow source
    std::string model_id;  // empty: automatic
    uint32_t queue_capacity = 4;
};

Result<void> validate(const Request& r, Diag* diag = nullptr);

}  // namespace rkvc
