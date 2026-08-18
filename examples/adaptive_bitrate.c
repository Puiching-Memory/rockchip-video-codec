/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file adaptive_bitrate.c
 * @brief 带宽自适应控制环：按输出字节数估算实际码率，运行中调 set_bitrate。
 *
 * 后台线程每秒对比估算码率与目标带宽，偏差超 15% 时步进 ±10% 并请求 IDR
 * 让解码端快速重同步。MPP H.264/HEVC 下一帧生效；SVT-AV1 运行中不生效。
 *
 * 用法: example_adaptive_bitrate [-d 设备|mock | -i in.nv12] [-o out.mp4]
 *                                 [-n 帧数] [-s WxH] [-b 目标bps]
 * 默认 mock 采集源，无摄像头/无输入文件也能演示完整控制环。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/time.h>

#define ADAPT_INTERVAL_MS 1000  /* 每秒调整一次 */
#define SMOOTH_ALPHA      0.3   /* 实际码率指数平滑系数 */

struct adapt_ctx {
    rkvc_session *session;
    int64_t       target_bps;
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
    double smoothed_bps = 0.0;

    /* 等待 session 启动 */
    while (a->running) {
        rkvc_session_stats st;
        if (rkvc_session_get_stats(a->session, &st) == RKVC_OK && st.running)
            break;
        usleep(100000);
    }

    int64_t prev_ts = now_us();
    rkvc_session_get_stats(a->session, &prev);

    while (a->running) {
        usleep((useconds_t)(ADAPT_INTERVAL_MS * 1000));

        rkvc_session_stats st;
        if (rkvc_session_get_stats(a->session, &st) != RKVC_OK)
            continue;

        int64_t dt_us = now_us() - prev_ts;
        if (dt_us <= 0)
            continue;

        double actual_bps = (double)((st.bytes_out - prev.bytes_out) * 8) * 1e6 /
                            (double)dt_us;
        smoothed_bps = smoothed_bps <= 0.0
                       ? actual_bps
                       : SMOOTH_ALPHA * actual_bps + (1.0 - SMOOTH_ALPHA) * smoothed_bps;

        /* 比例控制器：按偏差 10% 调整 */
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

        if (new_bps != a->target_bps) {
            rkvc_session_set_bitrate(a->session, new_bps);
            /* 码率突变后紧跟一个 IDR，解码端无需等 GOP 边界即可重同步 */
            rkvc_session_request_idr(a->session);
            printf("adaptive: actual=%.0f bps -> set=%lld bps +IDR\n",
                   smoothed_bps, (long long)new_bps);
        }

        prev = st;
        prev_ts = now_us();
    }

    return NULL;
}

int main(int argc, char **argv)
{
    const char *input = NULL;
    const char *dev = NULL;
    const char *out = "/tmp/rkvc_adaptive.mp4";
    int frames = 300;
    int w = 640, h = 480;
    int64_t target_bps = 2000000;

    int c;
    static struct option opts[] = {
        { "device",  required_argument, 0, 'd' },
        { "input",   required_argument, 0, 'i' },
        { "output",  required_argument, 0, 'o' },
        { "frames",  required_argument, 0, 'n' },
        { "size",    required_argument, 0, 's' },
        { "bitrate", required_argument, 0, 'b' },
        { 0, 0, 0, 0 }
    };
    while ((c = getopt_long(argc, argv, "d:i:o:n:s:b:", opts, NULL)) != -1) {
        if (c == 'd') dev = optarg;
        else if (c == 'i') input = optarg;
        else if (c == 'o') out = optarg;
        else if (c == 'n') frames = atoi(optarg);
        else if (c == 'b') target_bps = atoll(optarg);
        else if (c == 's') {
            int tw = 0, th = 0;
            char extra;
            if (sscanf(optarg, "%dx%d%c", &tw, &th, &extra) != 2 || tw <= 0 || th <= 0) {
                fprintf(stderr, "invalid size: %s (expected WxH)\n", optarg);
                return 1;
            }
            w = tw;
            h = th;
        }
    }

    if (input && dev) {
        fprintf(stderr, "-i 与 -d 互斥\n");
        return 1;
    }

    rkvc_pipeline_desc d;
    if (input) {
        rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
        d.input_path = input;
    } else {
        rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d);
        d.capture_device = dev ? dev : "mock";
        d.capture_max_frames = frames;
    }
    d.output_path = out;
    d.width = w;
    d.height = h;
    d.bitrate = target_bps;
    d.policy = RKVC_POLICY_REALTIME;

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
           input ? input : d.capture_device, out,
           (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out,
           (unsigned long long)st.bytes_out,
           rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
