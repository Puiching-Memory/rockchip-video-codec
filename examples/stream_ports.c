/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file stream_ports.c
 * @brief 命名端口流式消费：边录 MP4 边从 "output" 端口实时取码流。
 *
 * FILE_ENCODE 在后台线程跑 rkvc_session_run_file；主线程并发拉取 output
 * 端口的码流包（rkvc_buffer_get_bitstream 读 pts/key_frame），另存为
 * Annex-B .h264。演示 AGAIN 超时等待与生产者结束后排空队列。
 *
 * 用法: example_stream_ports [out.h264]
 */

#include "rkvc/rkvc.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH   320
#define HEIGHT  240
#define FRAMES  48
#define GOP     12

struct producer {
    rkvc_session *session;
    rkvc_err      err;
    int           done;
};

static void *run_thread(void *arg)
{
    struct producer *p = arg;
    p->err = rkvc_session_run_file(p->session);
    p->done = 1;
    return NULL;
}

/* 生成 NV12 测试剪辑，返回临时文件路径 */
static const char *make_input(void)
{
    static char path[] = "/tmp/rkvc_ports_in_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return NULL;

    size_t frame_sz = (size_t)WIDTH * HEIGHT * 3 / 2;
    uint8_t *raw = malloc(frame_sz);
    for (int i = 0; i < FRAMES; i++) {
        memset(raw, (uint8_t)(i & 0xff), (size_t)WIDTH * HEIGHT);
        memset(raw + (size_t)WIDTH * HEIGHT, 128, (size_t)WIDTH * HEIGHT / 2);
        if (write(fd, raw, frame_sz) != (ssize_t)frame_sz) {
            close(fd);
            free(raw);
            unlink(path);
            return NULL;
        }
    }
    close(fd);
    free(raw);
    return path;
}

int main(int argc, char **argv)
{
    const char *out_h264 = argc > 1 ? argv[1] : "/tmp/rkvc_ports.h264";
    const char *in_path = make_input();
    if (!in_path) {
        perror("make_input");
        return 1;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path  = in_path;
    d.output_path = "/tmp/rkvc_ports.mp4"; /* 封装照常写盘，端口是旁路 */
    d.width = WIDTH;
    d.height = HEIGHT;
    d.bitrate = 400000;
    d.gop_size = GOP;
    d.queue_depth = 8; /* 消费者慢时端口队列会丢包（frames_dropped 记账），调深可缓解 */
    d.policy = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        unlink(in_path);
        return 1;
    }

    rkvc_port *output = rkvc_session_port(s, "output");
    FILE *fp = fopen(out_h264, "wb");
    if (!output || !fp) {
        fprintf(stderr, "output port/file unavailable\n");
        rkvc_session_destroy(s);
        unlink(in_path);
        return 1;
    }

    struct producer p = { .session = s, .err = RKVC_OK, .done = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, run_thread, &p);

    int tapped = 0, keyframes = 0;
    for (;;) {
        rkvc_buffer *pkt = NULL;
        /* 生产中最多等 200ms；生产结束后非阻塞排空剩余队列 */
        err = rkvc_port_pull(output, &pkt, p.done ? 0 : 200);
        if (err == RKVC_ERR_AGAIN) {
            if (p.done)
                break;
            continue;
        }
        if (err != RKVC_OK) {
            fprintf(stderr, "port pull: %s\n", rkvc_err_str(err));
            break;
        }

        rkvc_buffer_bitstream_view v;
        if (rkvc_buffer_get_bitstream(pkt, &v) == RKVC_OK) {
            fwrite(v.data, 1, v.size, fp);
            tapped++;
            if (v.key_frame)
                keyframes++;
        }
        rkvc_buffer_unref(pkt);
    }

    pthread_join(tid, NULL);
    fclose(fp);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("stream_ports -> %s tapped=%d keyframes=%d session_out=%llu dropped=%llu err=%s\n",
           out_h264, tapped, keyframes,
           (unsigned long long)st.frames_out,
           (unsigned long long)st.frames_dropped,
           rkvc_err_str(p.err));

    rkvc_session_destroy(s);
    unlink(in_path);
    return p.err == RKVC_OK ? 0 : 1;
}
