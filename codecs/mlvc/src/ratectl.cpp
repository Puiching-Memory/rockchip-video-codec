// SPDX-License-Identifier: AGPL-3.0-or-later
#include "mlvc/ratectl.hpp"

#include <cmath>
#include <new>
#include <utility>

namespace mlvc {

namespace {

constexpr double kBucketSecs = 1.0;
constexpr double kInitLevel = 0.1;
constexpr double kTargetLevel = 0.1;
constexpr double kOvershootTau = 0.25;
constexpr double kUndershootTau = 1.0;
constexpr double kPlannedExcessTau = 0.5;

}  // namespace

void RateController::lb_init(LeakyBucket& b, double bitrate, double fps) {
    b = LeakyBucket{};
    b.bitrate = bitrate;
    b.fps = fps;
    b.capacity_bits = static_cast<int64_t>(bitrate * kBucketSecs);
    b.fill_bits =
        static_cast<int64_t>(kInitLevel * static_cast<double>(b.capacity_bits));
}

void RateController::rq_init(Rq& m, double alpha0, double beta, bool have_ramp,
                             double ramp_target, double ramp_duration) {
    m = Rq{};
    m.initial_alpha = alpha0;
    m.beta = beta;
    m.have_beta_ramp = have_ramp;
    m.beta_ramp_target = ramp_target;
    m.beta_ramp_duration = ramp_duration;
    m.seed_alpha = alpha0;
    m.alpha = alpha0;
    m.cur_beta = beta;
}

rkvc::Result<std::unique_ptr<RateController>> RateController::create(
    uint32_t width, uint32_t height, double bitrate_bps, double fps,
    rkvc::Diag* diag) {
    using R = rkvc::Result<std::unique_ptr<RateController>>;
    if (!width || !height || bitrate_bps <= 0.0 || fps <= 0.0) {
        if (diag)
            diag->add("create", "ratectl", "bad geometry or rate");
        return R::failure(rkvc::Status::Invalid, diag ? *diag : rkvc::Diag{});
    }
    std::unique_ptr<RateController> rc(new (std::nothrow) RateController());
    if (!rc)
        return R::failure(rkvc::Status::Nomem);
    rc->width_ = width;
    rc->height_ = height;
    lb_init(rc->alloc_.bucket, bitrate_bps, fps);
    rq_init(rc->m_i_, 0.04969, -0.03626, false, 0.0, 0.0);
    rq_init(rc->m_ltr_, 0.01047, -0.05306, false, 0.0, 0.0);
    rq_init(rc->m_p_idr_, 0.02156, -0.03173, true, -0.07654, 9.0);
    rq_init(rc->m_p_ltr_, 0.00315, -0.05925, true, -0.08307, 9.0);
    return R::success(std::move(rc));
}

void RateController::configure(double bitrate_bps) {
    if (bitrate_bps <= 0.0)
        return;
    LeakyBucket& b = alloc_.bucket;
    int64_t excess = b.fill_bits - static_cast<int64_t>(
                                        kInitLevel * b.capacity_bits);
    int64_t new_capacity = static_cast<int64_t>(bitrate_bps * kBucketSecs);
    int64_t new_fill =
        static_cast<int64_t>(kInitLevel * new_capacity) + excess;
    if (new_fill < 0)
        new_fill = 0;
    if (new_fill > new_capacity)
        new_fill = new_capacity;
    b.bitrate = bitrate_bps;
    b.capacity_bits = new_capacity;
    b.fill_bits = new_fill;
}

int64_t RateController::lb_drain_bits(const LeakyBucket& b, double pt) {
    double secs = b.have_last_ts ? pt - b.last_drain_ts : 1.0 / b.fps;
    int64_t drain = static_cast<int64_t>(secs * b.bitrate);
    return drain < b.fill_bits ? drain : b.fill_bits;
}

int64_t RateController::lb_calc_fill(const LeakyBucket& b, double pt) {
    return b.fill_bits - lb_drain_bits(b, pt);
}

void RateController::lb_update(LeakyBucket& b, double pt, int64_t frame_bits) {
    b.fill_bits = lb_calc_fill(b, pt) + frame_bits;
    b.last_drain_ts = pt;
    b.have_last_ts = true;
}

int64_t RateController::ra_planned_excess(Allocator& a, double pt,
                                          double weight) {
    double drain_secs = a.bucket.have_last_ts
                            ? pt - a.bucket.last_drain_ts
                            : 1.0 / a.bucket.fps;
    int64_t decay = static_cast<int64_t>((drain_secs / kPlannedExcessTau) *
                                         a.accum_excess_bits);
    int64_t excess = static_cast<int64_t>(
        (weight - 1.0) * (a.bucket.bitrate / a.bucket.fps));
    int64_t max_excess =
        static_cast<int64_t>(0.5 * static_cast<double>(a.bucket.capacity_bits));
    if (excess < 0)
        excess = 0;
    int64_t v = a.accum_excess_bits - decay + excess;
    if (v < 0)
        v = 0;
    if (v > max_excess)
        v = max_excess;
    return v;
}

int64_t RateController::ra_allocate(Allocator& a, double pt, double weight,
                                    double undershoot_tau_override) {
    LeakyBucket& b = a.bucket;
    int64_t nominal =
        static_cast<int64_t>(weight * (b.bitrate / b.fps));
    int64_t planned_excess = ra_planned_excess(a, pt, weight);
    int64_t target_fill = static_cast<int64_t>(kTargetLevel * b.capacity_bits) +
                          planned_excess;
    int64_t current_fill = lb_calc_fill(b, pt);
    int64_t error_bits = target_fill - (current_fill + nominal);
    double tau = error_bits >= 0
                     ? (undershoot_tau_override > 0.0 ? undershoot_tau_override
                                                     : kUndershootTau)
                     : kOvershootTau;
    int64_t correction =
        static_cast<int64_t>((1.0 / (tau * b.fps)) * error_bits);
    int64_t bucket_max =
        static_cast<int64_t>(0.9 * static_cast<double>(b.capacity_bits));
    int64_t headroom = bucket_max - current_fill;
    int64_t allocated = nominal + correction;
    if (allocated > headroom)
        allocated = headroom;
    int64_t lo = static_cast<int64_t>(0.33 * nominal);
    int64_t hi = static_cast<int64_t>(2.0 * nominal);
    if (allocated < lo)
        allocated = lo;
    if (allocated > hi)
        allocated = hi;
    if (current_fill + allocated > bucket_max)
        allocated = 0;
    return allocated;
}

void RateController::ra_update(Allocator& a, double pt, double weight,
                               int64_t frame_bits) {
    a.accum_excess_bits = ra_planned_excess(a, pt, weight);
    lb_update(a.bucket, pt, frame_bits);
}

void RateController::rq_reset(Rq& m) noexcept {
    m.alpha = m.seed_alpha;
    m.cur_beta = m.beta;
    m.num_updates = 0;
}

int RateController::rq_solve_q(const Rq& m, double bpp) noexcept {
    double q = -log((bpp > 1e-9 ? bpp : 1e-9) / m.alpha) / m.cur_beta;
    q = nearbyint(q);
    if (q < 0)
        q = 0;
    if (q > 63)
        q = 63;
    return static_cast<int>(q);
}

void RateController::rq_update(Rq& m, int q_index, double bpp) noexcept {
    double observed_alpha = bpp * exp(m.cur_beta * q_index);
    m.alpha += (1.0 / m.alpha_tau) * (observed_alpha - m.alpha);
    if (m.num_updates == 0)
        m.seed_alpha +=
            (1.0 / m.seed_alpha_tau) * (observed_alpha - m.seed_alpha);
    m.num_updates++;
    if (m.have_beta_ramp) {
        double progress =
            m.num_updates / (m.beta_ramp_duration > 1.0
                                 ? m.beta_ramp_duration
                                 : 1.0);
        if (progress > 1.0)
            progress = 1.0;
        double new_beta =
            m.beta + progress * (m.beta_ramp_target - m.beta);
        m.alpha *= exp((new_beta - m.cur_beta) * q_index);
        m.cur_beta = new_beta;
    }
}

RateController::Rq* RateController::model_for(RcFrameType t) noexcept {
    if (t == RcFrameType::I)
        return &m_i_;
    if (t == RcFrameType::LtrRecovery)
        return &m_ltr_;
    return last_recovery_ == RcFrameType::I ? &m_p_idr_ : &m_p_ltr_;
}

double RateController::frame_weight(RcFrameType t) noexcept {
    double w = prev_weight_ + (1.0 / 2.0) * (1.0 - prev_weight_);
    double type_w =
        t == RcFrameType::I ? 10.0 : t == RcFrameType::LtrRecovery ? 6.0 : 1.0;
    if (type_w > w)
        w = type_w;
    return w;
}

int RateController::solve(double presentation_time, RcFrameType frame_type,
                          int64_t reserved_overhead_bits) {
    double weight = frame_weight(frame_type);
    double undershoot_tau =
        (frame_type == RcFrameType::I ||
         frame_type == RcFrameType::LtrRecovery)
            ? 100.0
            : -1.0;
    int64_t allocated =
        ra_allocate(alloc_, presentation_time, weight, undershoot_tau);
    double target_bpp = static_cast<double>(allocated - reserved_overhead_bits);
    if (target_bpp < 0.0)
        target_bpp = 0.0;
    target_bpp /= static_cast<double>(width_) * height_;
    int q = rq_solve_q(*model_for(frame_type), target_bpp);
    cur_pt_ = presentation_time;
    cur_type_ = frame_type;
    cur_weight_ = weight;
    cur_allocated_ = allocated;
    cur_q_ = q;
    have_pending_ = true;
    (void)cur_allocated_;
    if (allocated <= 0)
        return kQDrop;
    return q;
}

void RateController::update(int64_t header_bits, int64_t payload_bits) {
    if (!have_pending_)
        return;
    have_pending_ = false;
    ra_update(alloc_, cur_pt_, cur_weight_, header_bits + payload_bits);
    if (payload_bits > 0) {
        double pixels = static_cast<double>(width_) * height_;
        rq_update(*model_for(cur_type_), cur_q_,
                  static_cast<double>(payload_bits) / pixels);
        if (cur_type_ == RcFrameType::I ||
            cur_type_ == RcFrameType::LtrRecovery) {
            rq_reset(m_p_idr_);
            rq_reset(m_p_ltr_);
            last_recovery_ = cur_type_;
        }
    }
    prev_weight_ = cur_weight_;
}

}  // namespace mlvc
