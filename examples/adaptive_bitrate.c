/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** adaptive_bitrate.c — V4L2 采集 + 实时码率自适应示例
 *
 *  每 ADAPT_INTERVAL_MS 根据输出字节数估算实际码率，与目标带宽比较后
 *  通过 rkvc_session_set_bitrate 调整目标码率。MPP H.264/HEVC 路径在下一帧
 *  编码前生效；SVT-AV1 仅更新 desc，运行中生效需重建 encoder。
 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#define ADAPT_INTERVAL_MS 1000  /* 每秒调整一次 */
#define SMOOTH_ALPHA      0.3   /* 实际码率指数平滑系数 */

struct adapt_ctx {
    rkvc_session *session;
    int64_t       target_bps;     /* 用户设定目标带宽 */
    int64_t       min_bps;
    int64_t       max_bps;
    int           running;
};

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static void *adapt_thread(void *arg)
{
    struct adapt_ctx *a = arg;
    rkvc_session_stats prev = {0};
    int64_t prev_ts = 0;
    double  smoothed_bps = 0.0;

    /* 等待 session 启动 */
    while (a->running) {
        rkvc_session_stats st;
        if (rkvc_session_get_stats(a->session, &st) == RKVC_OK && st.running)
            break;
        usleep(100000);
    }

    prev_ts = now_us();
    rkvc_session_get_stats(a->session, &prev);

    while (a->running) {
        usleep((useconds_t)(ADAPT_INTERVAL_MS * 1000));

        rkvc_session_stats st;
        if (rkvc_session_get_stats(a->session, &st) != RKVC_OK)
            continue;

        int64_t dt_us = now_us() - prev_ts;
        if (dt_us <= 0)
            continue;

        uint64_t bytes_delta = st.bytes_out - prev.bytes_out;
        double actual_bps = (double)(bytes_delta * 8) * 1e6 / (double)dt_us;

        if (smoothed_bps <= 0.0)
            smoothed_bps = actual_bps;
        else
            smoothed_bps = SMOOTH_ALPHA * actual_bps + (1.0 - SMOOTH_ALPHA) * smoothed_bps;

        /* 比例控制器：按偏差 10% 调整，每 1s 步进一次 */
        double ratio = smoothed_bps / (double)a->target_bps;
        int64_t new_bps = a->target_bps;
        if (ratio > 1.15)
            new_bps = (int64_t)(a->target_bps * 0.90);
        else if (ratio < 0.85)
            new_bps = (int64_t)(a->target_bps * 1.10);

        if (new_bps < a->min_bps)
            new_bps = a->min_bps;
        if (new_bps > a->max_bps)
            new_bps = a->max_bps;

        rkvc_session_set_bitrate(a->session, new_bps);

        printf("adaptive: actual=%.0f bps target=%lld bps set=%lld bps\n",
               smoothed_bps, (long long)a->target_bps, (long long)new_bps);

        prev    = st;
        prev_ts = now_us();
    }

    return NULL;
}

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video-camera0";
    const char *out = argc > 2 ? argv[2] : "/tmp/rkvc_adaptive.mp4";
    int frames      = argc > 3 ? atoi(argv[3]) : 300;
    int w = 640, h = 480;
    if (argc > 4)
        sscanf(argv[4], "%dx%d", &w, &h);

    int64_t target_bps = argc > 5 ? atoll(argv[5]) : 2000000;

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d);
    d.capture_device = dev;
    d.output_path    = out;
    d.width  = w;
    d.height = h;
    d.capture_max_frames = frames;
    d.bitrate = target_bps;
    d.policy  = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "create: %s\n", rkvc_err_str(err));
        return 1;
    }

    struct adapt_ctx a = {
        .session    = s,
        .target_bps = target_bps,
        .min_bps    = target_bps / 4,
        .max_bps    = target_bps * 2,
        .running    = 1,
    };

    pthread_t tid;
    pthread_create(&tid, NULL, adapt_thread, &a);

    err = rkvc_session_run_file(s);

    a.running = 0;
    pthread_join(tid, NULL);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("adaptive %s -> %s frames_in=%llu frames_out=%llu bytes_out=%llu err=%s\n",
           dev, out,
           (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out,
           (unsigned long long)st.bytes_out,
           rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
