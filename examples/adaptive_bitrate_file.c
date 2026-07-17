/** adaptive_bitrate_file.c — 文件转码 + 实时码率自适应示例
 *
 *  与 adaptive_bitrate.c 逻辑相同，但以文件为输入，无需摄像头权限。
 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/time.h>

#define ADAPT_INTERVAL_MS 1000
#define SMOOTH_ALPHA      0.3

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
    int64_t prev_ts = 0;
    double  smoothed_bps = 0.0;

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
    if (argc < 4) {
        fprintf(stderr, "usage: %s input out.mp4 target_bps [max_frames]\n", argv[0]);
        return 1;
    }

    const char *input = argv[1];
    const char *out   = argv[2];
    int64_t target_bps = atoll(argv[3]);
    int max_frames = argc > 4 ? atoi(argv[4]) : 300;

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    d.input_path  = input;
    d.output_path = out;
    d.bitrate     = target_bps;
    d.policy      = RKVC_POLICY_REALTIME;
    d.capture_max_frames = max_frames;

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
    printf("adaptive_file %s -> %s frames_in=%llu frames_out=%llu bytes_out=%llu err=%s\n",
           input, out,
           (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out,
           (unsigned long long)st.bytes_out,
           rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
