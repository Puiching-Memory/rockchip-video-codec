/* 最小视频编码样板：经 ais_semantic_codec → rkvc 后端编码 N 帧 NV12，
 * flush 后排空码流并打印包数与字节数。
 *
 * 环境变量（沿用 SDK 约定）：
 *   AIS_VIDEO_FAMILY       h264|hevc|av1|mlvc（默认 av1）
 *   AIS_VIDEO_FRAMES       帧数（默认 5）
 *   AIS_VIDEO_WIDTH/HEIGHT 帧尺寸（默认 64x64）
 *   AIS_VIDEO_BACKEND_DIR  可选：插件目录；不设则走 rkvc 默认搜索路径
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ais/ais.h"

static int env_int(const char* name, int dflt) {
    const char* s = getenv(name);
    const int v = s ? atoi(s) : 0;
    return v > 0 ? v : dflt;
}

static ais_codec_family_t env_family(void) {
    const char* s = getenv("AIS_VIDEO_FAMILY");
    if (!s)
        return AIS_CODEC_FAMILY_AV1;
    if (strcmp(s, "h264") == 0)
        return AIS_CODEC_FAMILY_H264;
    if (strcmp(s, "hevc") == 0)
        return AIS_CODEC_FAMILY_HEVC;
    if (strcmp(s, "mlvc") == 0)
        return AIS_CODEC_FAMILY_MLVC;
    return AIS_CODEC_FAMILY_AV1;
}

int main(void) {
    const int frames = env_int("AIS_VIDEO_FRAMES", 5);
    const uint32_t width = (uint32_t)env_int("AIS_VIDEO_WIDTH", 64);
    const uint32_t height = (uint32_t)env_int("AIS_VIDEO_HEIGHT", 64);
    ais_video_caps_t caps;
    ais_video_config_t cfg;
    ais_video_codec_t* enc = NULL;
    ais_status_t st;
    int i, packets = 0;
    size_t bytes = 0;

    if (ais_video_caps(&caps) == AIS_OK)
        printf("caps: soc=%.63s enc=%u dec=%u npu=%u\n", caps.soc,
               caps.has_encoder, caps.has_decoder, caps.has_npu);

    memset(&cfg, 0, sizeof(cfg));
    cfg.id = AIS_CODEC_VIDEO_STANDARD;
    cfg.mode = AIS_ENCODE;
    cfg.family = env_family();
    cfg.policy = AIS_VIDEO_POLICY_REALTIME;
    cfg.width = width;
    cfg.height = height;
    cfg.format = AIS_PIXEL_NV12;
    cfg.fps_num = 30;
    cfg.fps_den = 1;
    cfg.bitrate_bps = 4000000;
    cfg.qp = -1;
    cfg.backend_dir = getenv("AIS_VIDEO_BACKEND_DIR");

    st = ais_video_open(&cfg, &enc);
    if (st == AIS_ERR_UNSUPPORTED) {
        printf("video codec not built here; nothing to do\n");
        return 0;
    }
    if (st != AIS_OK) {
        fprintf(stderr, "ais_video_open: %s\n", ais_status_str(st));
        return 1;
    }

    for (i = 0; i < frames; ++i) {
        ais_buffer_t* frame = NULL;
        st = ais_buffer_video(width, height, AIS_PIXEL_NV12, (int64_t)i * 33333,
                              (uint32_t)i, &frame);
        if (st == AIS_OK) {
            memset(ais_buffer_data(frame), 0x22 + (i & 0x0f),
                   ais_buffer_size(frame));
            st = ais_video_send(enc, frame);
        }
        ais_buffer_destroy(frame);
        if (st != AIS_OK) {
            fprintf(stderr, "send frame %d: %s\n", i, ais_status_str(st));
            ais_video_close(enc);
            return 1;
        }
    }

    st = ais_video_flush(enc);
    if (st != AIS_OK)
        fprintf(stderr, "flush: %s\n", ais_status_str(st));

    for (;;) {
        ais_buffer_t* packet = NULL;
        st = ais_video_recv(enc, &packet);
        if (st == AIS_ERR_EOF)
            break;
        if (st != AIS_OK) {
            fprintf(stderr, "recv: %s\n", ais_status_str(st));
            break;
        }
        bytes += ais_buffer_size(packet);
        ++packets;
        ais_buffer_destroy(packet);
    }
    ais_video_close(enc);

    printf("encode: %d frames -> %d packets, %lu bytes\n", frames, packets,
           (unsigned long)bytes);
    return (packets > 0 && bytes > 0) ? 0 : 1;
}
