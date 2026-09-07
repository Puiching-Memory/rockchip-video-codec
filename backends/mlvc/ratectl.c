/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file ratectl.c
 * @brief MLVC 闭环码率控制——microsoft/mlvc video/src/utils/rate_controller.py
 *        的逐行 C 移植（默认参数、取整口径、状态机一致）。
 */

#include "ratectl.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ── LeakyBucket ─────────────────────────────────────────────────── */

typedef struct {
    double bitrate;        /* bits/s */
    double fps;
    double bucket_size;    /* 秒，恒 1.0 */
    double initial_level;  /* 0.1 */
    int64_t capacity_bits;
    int64_t fill_bits;
    int have_last_ts;
    double last_drain_ts;
} mlvc_lb;

static void lb_init(mlvc_lb *b, double bitrate, double fps)
{
    memset(b, 0, sizeof(*b));
    b->bitrate = bitrate;
    b->fps = fps;
    b->bucket_size = 1.0;
    b->initial_level = 0.1;
    b->capacity_bits = (int64_t)(bitrate * b->bucket_size);
    b->fill_bits = (int64_t)(b->initial_level * (double)b->capacity_bits);
}

static void lb_configure(mlvc_lb *b, double bitrate)
{
    /* 保留相对目标水位的偏差 */
    int64_t excess = b->fill_bits - (int64_t)(b->initial_level * (double)b->capacity_bits);
    int64_t new_capacity = (int64_t)(bitrate * b->bucket_size);
    int64_t new_fill = (int64_t)(b->initial_level * (double)new_capacity) + excess;
    if (new_fill < 0)
        new_fill = 0;
    if (new_fill > new_capacity)
        new_fill = new_capacity;
    b->bitrate = bitrate;
    b->capacity_bits = new_capacity;
    b->fill_bits = new_fill;
}

static double lb_drain_secs(const mlvc_lb *b, double pt)
{
    if (!b->have_last_ts)
        return 1.0 / b->fps;
    return pt - b->last_drain_ts;
}

static int64_t lb_drain_bits(const mlvc_lb *b, double pt)
{
    int64_t drain = (int64_t)(lb_drain_secs(b, pt) * b->bitrate);
    return drain < b->fill_bits ? drain : b->fill_bits;
}

static int64_t lb_calc_fill(const mlvc_lb *b, double pt)
{
    return b->fill_bits - lb_drain_bits(b, pt);
}

static void lb_update(mlvc_lb *b, double pt, int64_t frame_bits)
{
    b->fill_bits = lb_calc_fill(b, pt) + frame_bits;
    b->last_drain_ts = pt;
    b->have_last_ts = 1;
}

/* ── RateAllocator ───────────────────────────────────────────────── */

typedef struct {
    double target_level;        /* 0.1 */
    double overshoot_tau;       /* 0.25 */
    double undershoot_tau;      /* 1.0 */
    double planned_excess_tau;  /* 0.5 */
    mlvc_lb bucket;
    int64_t accum_excess_bits;
} mlvc_ra;

static void ra_init(mlvc_ra *a, double bitrate, double fps)
{
    memset(a, 0, sizeof(*a));
    a->target_level = 0.1;
    a->overshoot_tau = 0.25;
    a->undershoot_tau = 1.0;
    a->planned_excess_tau = 0.5;
    lb_init(&a->bucket, bitrate, fps);
}

static int64_t ra_planned_excess(mlvc_ra *a, double pt, double weight)
{
    double drain_secs = lb_drain_secs(&a->bucket, pt);
    int64_t decay = (int64_t)((drain_secs / a->planned_excess_tau) * (double)a->accum_excess_bits);
    int64_t excess = (int64_t)((weight - 1.0) * (a->bucket.bitrate / a->bucket.fps));
    int64_t max_excess = (int64_t)(0.5 * (double)a->bucket.capacity_bits);
    int64_t v;
    if (excess < 0)
        excess = 0;
    v = a->accum_excess_bits - decay + excess;
    if (v < 0)
        v = 0;
    if (v > max_excess)
        v = max_excess;
    return v;
}

/** allocate 结果；allocated<=0 表示丢帧。 */
static int64_t ra_allocate(mlvc_ra *a, double pt, double weight,
                           double undershoot_tau_override)
{
    mlvc_lb *b = &a->bucket;
    int64_t nominal = (int64_t)(weight * (b->bitrate / b->fps));
    int64_t planned_excess = ra_planned_excess(a, pt, weight);
    int64_t target_fill = (int64_t)(a->target_level * (double)b->capacity_bits) + planned_excess;
    int64_t current_fill = lb_calc_fill(b, pt);
    int64_t error_bits = target_fill - (current_fill + nominal);
    double tau = error_bits >= 0
        ? (undershoot_tau_override > 0.0 ? undershoot_tau_override : a->undershoot_tau)
        : a->overshoot_tau;
    int64_t correction = (int64_t)((1.0 / (tau * b->fps)) * (double)error_bits);
    int64_t bucket_max = (int64_t)(0.9 * (double)b->capacity_bits);
    int64_t headroom = bucket_max - current_fill;
    int64_t allocated = nominal + correction;
    int64_t lo, hi;

    if (allocated > headroom)
        allocated = headroom;
    lo = (int64_t)(0.33 * (double)nominal);
    hi = (int64_t)(2.0 * (double)nominal);
    if (allocated < lo)
        allocated = lo;
    if (allocated > hi)
        allocated = hi;

    if (current_fill + allocated > bucket_max)
        allocated = 0;
    return allocated;
}

static void ra_update(mlvc_ra *a, double pt, double weight, int64_t frame_bits)
{
    a->accum_excess_bits = ra_planned_excess(a, pt, weight);
    lb_update(&a->bucket, pt, frame_bits);
}

/* ── RateModel（指数 R-Q） ───────────────────────────────────────── */

typedef struct {
    double initial_alpha;
    double beta;
    double alpha_tau;
    double seed_alpha_tau;
    int have_beta_ramp;
    double beta_ramp_target;
    double beta_ramp_duration;
    int min_q, max_q;
    /* 运行态 */
    double seed_alpha;
    double alpha;
    double cur_beta;
    int num_updates;
} mlvc_rq;

static void rq_reset(mlvc_rq *m)
{
    m->alpha = m->seed_alpha;
    m->cur_beta = m->beta;
    m->num_updates = 0;
}

static void rq_init(mlvc_rq *m, double alpha0, double beta, double alpha_tau,
                    int have_ramp, double ramp_target, double ramp_duration)
{
    memset(m, 0, sizeof(*m));
    m->initial_alpha = alpha0;
    m->beta = beta;
    m->alpha_tau = alpha_tau;
    m->seed_alpha_tau = 2.0;
    m->have_beta_ramp = have_ramp;
    m->beta_ramp_target = ramp_target;
    m->beta_ramp_duration = ramp_duration;
    m->min_q = 0;
    m->max_q = 63;
    m->seed_alpha = alpha0;
    rq_reset(m);
}

static int rq_solve_q(const mlvc_rq *m, double bpp)
{
    double q = -log((bpp > 1e-9 ? bpp : 1e-9) / m->alpha) / m->cur_beta;
    /* np.round = half-to-even，对应默认舍入模式的 nearbyint */
    q = nearbyint(q);
    if (q < m->min_q)
        q = m->min_q;
    if (q > m->max_q)
        q = m->max_q;
    return (int)q;
}

static void rq_update(mlvc_rq *m, int q_index, double bpp)
{
    double observed_alpha = bpp * exp(m->cur_beta * (double)q_index);
    m->alpha += (1.0 / m->alpha_tau) * (observed_alpha - m->alpha);
    if (m->num_updates == 0)
        m->seed_alpha += (1.0 / m->seed_alpha_tau) * (observed_alpha - m->seed_alpha);
    m->num_updates++;
    if (m->have_beta_ramp) {
        double progress = (double)m->num_updates / (m->beta_ramp_duration > 1.0 ? m->beta_ramp_duration : 1.0);
        double new_beta;
        if (progress > 1.0)
            progress = 1.0;
        new_beta = m->beta + progress * (m->beta_ramp_target - m->beta);
        /* 保持 alpha*exp(-beta*q) 不变 */
        m->alpha *= exp((new_beta - m->cur_beta) * (double)q_index);
        m->cur_beta = new_beta;
    }
}

/* ── RateController ──────────────────────────────────────────────── */

struct mlvc_rate_controller {
    uint32_t width, height;
    mlvc_ra alloc;
    mlvc_rq m_i;             /* I_FRAME */
    mlvc_rq m_ltr;           /* LTR_RECOVERY */
    mlvc_rq m_p_idr;         /* P after IDR */
    mlvc_rq m_p_ltr;         /* P after LTR_RECOVERY */
    mlvc_rc_frame_type last_recovery; /* I 或 LTR_RECOVERY */
    /* 当前帧状态（solve 暂存，update 消费） */
    double cur_pt;
    mlvc_rc_frame_type cur_type;
    double cur_weight;
    int64_t cur_allocated;
    int cur_q;
    int have_pending;
    /* 上一帧状态 */
    double prev_weight;
    int prev_q;
    int have_prev_q;
};

static mlvc_rq *rc_model_for(mlvc_rate_controller *rc, mlvc_rc_frame_type t)
{
    if (t == MLVC_RC_FRAME_I)
        return &rc->m_i;
    if (t == MLVC_RC_FRAME_LTR_RECOVERY)
        return &rc->m_ltr;
    return rc->last_recovery == MLVC_RC_FRAME_I ? &rc->m_p_idr : &rc->m_p_ltr;
}

mlvc_rate_controller *mlvc_rc_create(uint32_t width, uint32_t height,
                                     double bitrate_bps, double fps)
{
    mlvc_rate_controller *rc;
    if (!width || !height || bitrate_bps <= 0.0 || fps <= 0.0)
        return NULL;
    rc = calloc(1, sizeof(*rc));
    if (!rc)
        return NULL;
    rc->width = width;
    rc->height = height;
    ra_init(&rc->alloc, bitrate_bps, fps);
    /* 官方拟合值 */
    rq_init(&rc->m_i, 0.04969, -0.03626, 2.0, 0, 0.0, 0.0);
    rq_init(&rc->m_ltr, 0.01047, -0.05306, 2.0, 0, 0.0, 0.0);
    rq_init(&rc->m_p_idr, 0.02156, -0.03173, 2.0, 1, -0.07654, 9.0);
    rq_init(&rc->m_p_ltr, 0.00315, -0.05925, 2.0, 1, -0.08307, 9.0);
    rc->last_recovery = MLVC_RC_FRAME_I;
    rc->prev_weight = 1.0;
    return rc;
}

void mlvc_rc_free(mlvc_rate_controller *rc)
{
    free(rc);
}

void mlvc_rc_configure(mlvc_rate_controller *rc, double bitrate_bps)
{
    if (!rc || bitrate_bps <= 0.0)
        return;
    lb_configure(&rc->alloc.bucket, bitrate_bps);
}

static double rc_frame_weight(mlvc_rate_controller *rc, mlvc_rc_frame_type t)
{
    /* 向 1.0 衰减，再被帧型权重抬升 */
    double w = rc->prev_weight + (1.0 / 2.0) * (1.0 - rc->prev_weight);
    double type_w = t == MLVC_RC_FRAME_I ? 10.0
        : t == MLVC_RC_FRAME_LTR_RECOVERY ? 6.0 : 1.0;
    if (type_w > w)
        w = type_w;
    return w;
}

int mlvc_rc_solve_q_index(mlvc_rate_controller *rc, double presentation_time,
                          mlvc_rc_frame_type frame_type,
                          int64_t reserved_overhead_bits)
{
    double weight, undershoot_tau, target_bpp;
    int64_t allocated;
    int q;

    if (!rc)
        return MLVC_RC_Q_DROP;
    weight = rc_frame_weight(rc, frame_type);
    /* I/LTR_RECOVERY 关闭 undershoot 修正（tau=100） */
    undershoot_tau = (frame_type == MLVC_RC_FRAME_I || frame_type == MLVC_RC_FRAME_LTR_RECOVERY)
        ? 100.0 : -1.0;
    allocated = ra_allocate(&rc->alloc, presentation_time, weight, undershoot_tau);

    target_bpp = (double)(allocated - reserved_overhead_bits);
    if (target_bpp < 0.0)
        target_bpp = 0.0;
    target_bpp /= (double)rc->width * (double)rc->height;
    q = rq_solve_q(rc_model_for(rc, frame_type), target_bpp);

    rc->cur_pt = presentation_time;
    rc->cur_type = frame_type;
    rc->cur_weight = weight;
    rc->cur_allocated = allocated;
    rc->cur_q = q;
    rc->have_pending = 1;

    if (allocated <= 0)
        return MLVC_RC_Q_DROP;
    return q;
}

void mlvc_rc_update(mlvc_rate_controller *rc, int64_t header_bits,
                    int64_t payload_bits)
{
    double pixels, payload_bpp;
    mlvc_rc_frame_type t;

    if (!rc || !rc->have_pending)
        return;
    rc->have_pending = 0;

    ra_update(&rc->alloc, rc->cur_pt, rc->cur_weight,
              header_bits + payload_bits);

    if (payload_bits <= 0) {
        /* 丢帧：不进模型 update（对齐官方） */
    } else {
        pixels = (double)rc->width * (double)rc->height;
        payload_bpp = (double)payload_bits / pixels;
        t = rc->cur_type;
        rq_update(rc_model_for(rc, t), rc->cur_q, payload_bpp);
        if (t == MLVC_RC_FRAME_I || t == MLVC_RC_FRAME_LTR_RECOVERY) {
            rq_reset(&rc->m_p_idr);
            rq_reset(&rc->m_p_ltr);
            rc->last_recovery = t;
        }
    }

    rc->prev_weight = rc->cur_weight;
    rc->prev_q = rc->cur_q;
    rc->have_prev_q = 1;
}
