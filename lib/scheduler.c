/**
 * @file scheduler.c
 * @brief 轻量调度辅助（会话 worker 线程入口）。
 */

#include "internal.h"

void rkvc_session_stats_tick(rkvc_session *s, int frame_out)
{
    if (!s)
        return;

    pthread_mutex_lock(&s->lock);
    if (frame_out)
        s->stats.frames_out++;

    if (s->first_ts_us == 0)
        s->first_ts_us = rkvc_now_us();

    int64_t elapsed = rkvc_now_us() - s->first_ts_us;
    if (elapsed > 0 && s->stats.frames_out > 0)
        s->stats.avg_fps = (double)s->stats.frames_out * 1e6 / (double)elapsed;
    pthread_mutex_unlock(&s->lock);
}

void rkvc_session_stats_frame_in(rkvc_session *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->stats.frames_in++;
    pthread_mutex_unlock(&s->lock);
}

void rkvc_session_stats_drop(rkvc_session *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->stats.frames_dropped++;
    pthread_mutex_unlock(&s->lock);
}
