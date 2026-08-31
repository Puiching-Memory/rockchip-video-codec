/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file live_transcode_ports.c
 * @brief capture 端口推 Annex-B access unit，output 端口实时拉硬编码流。
 *
 * 本示例用 libavformat 代替业务侧 demux。Monibuca 集成时，把 av_read_frame
 * 循环替换为其视频包回调即可；rkvc 侧不需要 input_path/output_path。
 *
 * 用法：example_live_transcode_ports input.{h264,h265} out.{h264,h265}
 *                                      [h264|hevc] [width height bitrate seconds]
 */

#include "rkvc/rkvc.h"

#include <libavformat/avformat.h>
#include <libavutil/time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    rkvc_port   *output;
    FILE        *fp;
    volatile int done;
    rkvc_err     err;
    uint64_t     packets;
} output_consumer;

static void *pull_output(void *opaque)
{
    output_consumer *c = opaque;
    c->err = RKVC_OK;
    for (;;) {
        rkvc_buffer *pkt = NULL;
        rkvc_err err = rkvc_port_pull(c->output, &pkt, c->done ? 0 : 100);
        if (err == RKVC_ERR_AGAIN) {
            if (c->done)
                break;
            continue;
        }
        if (err == RKVC_ERR_EOF)
            break;
        if (err != RKVC_OK) {
            c->err = err;
            break;
        }

        rkvc_buffer_bitstream_view view;
        err = rkvc_buffer_get_bitstream(pkt, &view);
        if (err == RKVC_OK &&
            fwrite(view.data, 1, view.size, c->fp) != view.size)
            err = RKVC_ERR_IO;
        rkvc_buffer_unref(pkt);
        if (err != RKVC_OK) {
            c->err = err;
            break;
        }
        c->packets++;
    }
    return NULL;
}

static rkvc_codec codec_from_av(enum AVCodecID id)
{
    if (id == AV_CODEC_ID_H264)
        return RKVC_CODEC_H264;
    if (id == AV_CODEC_ID_HEVC)
        return RKVC_CODEC_HEVC;
    return RKVC_CODEC_AUTO;
}

static rkvc_codec parse_target(const char *name)
{
    if (!name || strcmp(name, "h264") == 0)
        return RKVC_CODEC_H264;
    if (strcmp(name, "hevc") == 0 || strcmp(name, "h265") == 0)
        return RKVC_CODEC_HEVC;
    return RKVC_CODEC_AUTO;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s input out.es [h264|hevc] "
                        "[width height bitrate seconds]\n", argv[0]);
        return 2;
    }

    avformat_network_init();
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0 ||
        avformat_find_stream_info(fmt, NULL) < 0) {
        fprintf(stderr, "cannot open input: %s\n", argv[1]);
        avformat_close_input(&fmt);
        return 1;
    }

    int video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (video < 0) {
        fprintf(stderr, "no video stream\n");
        avformat_close_input(&fmt);
        return 1;
    }

    AVCodecParameters *par = fmt->streams[video]->codecpar;
    rkvc_codec input_codec = codec_from_av(par->codec_id);
    rkvc_codec target_codec = parse_target(argc > 3 ? argv[3] : NULL);
    if (input_codec == RKVC_CODEC_AUTO || target_codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "only H.264/H.265 input and output are supported\n");
        avformat_close_input(&fmt);
        return 2;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_TRANSCODE, &d);
    d.input_codec = input_codec;
    d.codec = target_codec;
    d.width = argc > 4 ? atoi(argv[4]) : par->width;
    d.height = argc > 5 ? atoi(argv[5]) : par->height;
    d.bitrate = argc > 6 ? atoll(argv[6]) : 4000000;
    int duration_sec = argc > 7 ? atoi(argv[7]) : 0;
    d.fps_num = 30;
    d.fps_den = 1;
    if (fmt->streams[video]->avg_frame_rate.num > 0 &&
        fmt->streams[video]->avg_frame_rate.den > 0) {
        d.fps_num = fmt->streams[video]->avg_frame_rate.num;
        d.fps_den = fmt->streams[video]->avg_frame_rate.den;
    }

    rkvc_session *session = NULL;
    rkvc_err err = rkvc_session_create(&d, &session);
    if (err == RKVC_OK)
        err = rkvc_session_start(session);
    if (err != RKVC_OK) {
        fprintf(stderr, "session start: %s\n", rkvc_err_str(err));
        rkvc_session_destroy(session);
        avformat_close_input(&fmt);
        return 1;
    }

    FILE *fp = fopen(argv[2], "wb");
    rkvc_port *capture = rkvc_session_port(session, "capture");
    rkvc_port *output = rkvc_session_port(session, "output");
    if (!fp || !capture || !output) {
        fprintf(stderr, "ports/output unavailable\n");
        if (fp)
            fclose(fp);
        rkvc_session_destroy(session);
        avformat_close_input(&fmt);
        return 1;
    }

    output_consumer consumer = { .output = output, .fp = fp };
    pthread_t pull_thread;
    if (pthread_create(&pull_thread, NULL, pull_output, &consumer) != 0) {
        fprintf(stderr, "cannot start output consumer\n");
        fclose(fp);
        rkvc_session_destroy(session);
        avformat_close_input(&fmt);
        return 1;
    }

    AVPacket *avpkt = av_packet_alloc();
    int64_t deadline_us = duration_sec > 0
                              ? av_gettime_relative() +
                                    (int64_t)duration_sec * 1000000
                              : 0;
    while (avpkt) {
        if (deadline_us > 0 && av_gettime_relative() >= deadline_us)
            break;
        if (av_read_frame(fmt, avpkt) < 0) {
            if (deadline_us > 0 && av_gettime_relative() < deadline_us &&
                av_seek_frame(fmt, video, 0, AVSEEK_FLAG_BACKWARD) >= 0) {
                avformat_flush(fmt);
                continue;
            }
            break;
        }
        if (avpkt->stream_index != video) {
            av_packet_unref(avpkt);
            continue;
        }

        rkvc_buffer *pkt = NULL;
        err = rkvc_buffer_alloc_bitstream(&pkt, avpkt->data,
                                          (size_t)avpkt->size, 1);
        if (err == RKVC_OK)
            rkvc_buffer_set_timestamps(pkt, avpkt->pts, avpkt->dts);
        while (err == RKVC_OK &&
               (err = rkvc_port_push(capture, pkt)) == RKVC_ERR_AGAIN)
            av_usleep(1000);
        rkvc_buffer_unref(pkt);
        av_packet_unref(avpkt);
        if (err != RKVC_OK)
            break;
    }
    av_packet_free(&avpkt);

    rkvc_err stop_err = rkvc_session_stop(session);
    consumer.done = 1;
    pthread_join(pull_thread, NULL);
    fclose(fp);

    rkvc_session_stats stats;
    rkvc_session_get_stats(session, &stats);
    printf("route: %s -> %s, in=%llu out=%llu pulled=%llu dropped=%llu\n",
           stats.route.dec_name, stats.route.enc_name,
           (unsigned long long)stats.frames_in,
           (unsigned long long)stats.frames_out,
           (unsigned long long)consumer.packets,
           (unsigned long long)stats.frames_dropped);

    rkvc_session_destroy(session);
    avformat_close_input(&fmt);
    avformat_network_deinit();
    if (err != RKVC_OK && err != RKVC_ERR_EOF)
        return 1;
    return stop_err == RKVC_OK && consumer.err == RKVC_OK ? 0 : 1;
}
