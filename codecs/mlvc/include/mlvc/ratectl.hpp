// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>

#include "rkvc/diag.hpp"
#include "rkvc/result.hpp"

namespace mlvc {

// Closed-loop rate control, a line-by-line port of the C implementation
// (itself a port of microsoft/mlvc rate_controller.py): identical defaults,
// rounding (C truncation, nearbyint half-to-even) and state machine.
// q_index: 0..63, higher is better (inverse of H.265 QP).
enum class RcFrameType : uint32_t {
    I = 0,
    P = 1,
    LtrRecovery = 2,
};

inline constexpr int kQDrop = -1;

class RateController {
public:
    static rkvc::Result<std::unique_ptr<RateController>> create(
        uint32_t width, uint32_t height, double bitrate_bps, double fps,
        rkvc::Diag* diag = nullptr);
    // Retune the target bitrate, keeping the level deviation.
    void configure(double bitrate_bps);
    // Next q_index, or kQDrop when the frame should be dropped.
    int solve(double presentation_time, RcFrameType type,
              int64_t reserved_overhead_bits);
    // Encode feedback. Dropped frames (solve returned kQDrop) pass
    // payload_bits 0: bucket only, models untouched.
    void update(int64_t header_bits, int64_t payload_bits);

private:
    struct LeakyBucket {
        double bitrate = 0;
        double fps = 0;
        int64_t capacity_bits = 0;
        int64_t fill_bits = 0;
        bool have_last_ts = false;
        double last_drain_ts = 0;
    };
    struct Allocator {
        LeakyBucket bucket;
        int64_t accum_excess_bits = 0;
    };
    struct Rq {
        double initial_alpha = 0;
        double beta = 0;
        double alpha_tau = 2;
        double seed_alpha_tau = 2;
        bool have_beta_ramp = false;
        double beta_ramp_target = 0;
        double beta_ramp_duration = 0;
        double seed_alpha = 0;
        double alpha = 0;
        double cur_beta = 0;
        int num_updates = 0;
    };

    RateController() = default;
    Rq* model_for(RcFrameType t) noexcept;
    double frame_weight(RcFrameType t) noexcept;
    static int64_t lb_drain_bits(const LeakyBucket& b, double pt);
    static int64_t lb_calc_fill(const LeakyBucket& b, double pt);
    static void lb_update(LeakyBucket& b, double pt, int64_t frame_bits);
    static int64_t ra_planned_excess(Allocator& a, double pt, double weight);
    static int64_t ra_allocate(Allocator& a, double pt, double weight,
                               double undershoot_tau_override);
    static void ra_update(Allocator& a, double pt, double weight,
                          int64_t frame_bits);
    static void lb_init(LeakyBucket& b, double bitrate, double fps);
    static void rq_init(Rq& m, double alpha0, double beta, bool have_ramp,
                        double ramp_target, double ramp_duration);
    static void rq_reset(Rq& m) noexcept;
    static int rq_solve_q(const Rq& m, double bpp) noexcept;
    static void rq_update(Rq& m, int q_index, double bpp) noexcept;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
    Allocator alloc_;
    Rq m_i_;
    Rq m_ltr_;
    Rq m_p_idr_;
    Rq m_p_ltr_;
    RcFrameType last_recovery_ = RcFrameType::I;
    double cur_pt_ = 0;
    RcFrameType cur_type_ = RcFrameType::P;
    double cur_weight_ = 0;
    int64_t cur_allocated_ = 0;
    int cur_q_ = 0;
    bool have_pending_ = false;
    double prev_weight_ = 1.0;
};

}  // namespace mlvc
