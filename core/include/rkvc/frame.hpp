// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"
#include "rkvc/spec.hpp"

namespace rkvc {

inline constexpr uint32_t kRoiMaxRegions = 8;
inline constexpr uint32_t kFlagKeyframe = 1u << 0;
inline constexpr uint32_t kFlagDiscontinuity = 1u << 1;
inline constexpr uint32_t kFlagCorrupt = 1u << 2;

struct RoiRegion {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    int16_t qp_delta = 0;
    bool force_intra = false;
};

struct EncodeControl {
    int32_t bitrate_bps = 0;
    uint32_t gop_size = 0;
    bool force_idr = false;
};

// Backend release hook, invoked when the last Frame reference drops.
struct FrameHooks {
    void (*release)(void* ctx) noexcept = nullptr;
    void* ctx = nullptr;
};

// Reference-counted media object. Borrowed payload outlives the Frame;
// the hook fires on last release (e.g. frees the sender-side copy).
class Frame {
public:
    static Result<std::shared_ptr<Frame>> borrow_host(const Spec& spec,
                                                      void* data, size_t size,
                                                      FrameHooks hooks = {},
                                                      Diag* diag = nullptr);
    static Result<std::shared_ptr<Frame>> borrow_dmabuf(const Spec& spec,
                                                        int fd, size_t size,
                                                        FrameHooks hooks = {},
                                                        Diag* diag = nullptr);

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    ~Frame();

    const Spec& spec() const noexcept { return spec_; }
    void* data() const noexcept { return data_; }
    size_t size() const noexcept { return size_; }
    int fd() const noexcept { return fd_; }
    int64_t pts() const noexcept { return pts_; }
    int64_t dts() const noexcept { return dts_; }
    uint32_t flags() const noexcept { return flags_; }
    void set_pts(int64_t v) noexcept { pts_ = v; }
    void set_dts(int64_t v) noexcept { dts_ = v; }
    void set_flags(uint32_t v) noexcept { flags_ = v; }
    void set_encode(EncodeControl e) noexcept { encode_ = e; }
    const EncodeControl& encode() const noexcept { return encode_; }
    Result<void> set_roi(const RoiRegion* regions, size_t count,
                         Diag* diag = nullptr);
    const RoiRegion* roi_regions() const noexcept { return roi_.data(); }
    size_t roi_count() const noexcept { return roi_count_; }

private:
    Frame(Spec spec, void* data, size_t size, int fd, FrameHooks hooks)
        : spec_(spec),
          data_(data),
          size_(size),
          fd_(fd),
          hooks_(hooks) {}

    Spec spec_;
    void* data_ = nullptr;
    size_t size_ = 0;
    int fd_ = -1;
    int64_t pts_ = kTsUnknown;
    int64_t dts_ = kTsUnknown;
    uint32_t flags_ = 0;
    EncodeControl encode_;
    std::array<RoiRegion, kRoiMaxRegions> roi_{};
    size_t roi_count_ = 0;
    FrameHooks hooks_;
};

}  // namespace rkvc
