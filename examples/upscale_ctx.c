



/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file upscale_ctx.c
 * @brief RGA 上采样：一次性 API 与复用 ctx 批量 API 的用法与开销对比。
 *
 * 一次性 `rkvc_upscale_nv12` 每帧都 import/release 缓冲；
 * `rkvc_upscale_ctx_*` 固定内部 DMA 缓冲反复缩放，适合批处理。
 *
 * 用法: example_upscale_ctx [帧数]
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define SRC_W 640
#define SRC_H 360
#define DST_W 1920
#define DST_H 1080

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

int main(int argc, char **argv)
{
    int frames = argc > 1 ? atoi(argv[1]) : 100;
    const size_t src_sz = (size_t)SRC_W * SRC_H * 3 / 2;
    const size_t dst_sz = (size_t)DST_W * DST_H * 3 / 2;

    uint8_t *src = malloc(src_sz);
    uint8_t *dst = malloc(dst_sz);
    if (!src || !dst) {
        free(src);
        free(dst);
        return 1;
    }
    /* 渐变灰度图案，UV 置 128 */
    for (int y = 0; y < SRC_H; y++)
        memset(src + (size_t)y * SRC_W, y * 255 / SRC_H, SRC_W);
    memset(src + (size_t)SRC_W * SRC_H, 128, (size_t)SRC_W * SRC_H / 2);

    /* 一次性 API：每帧独立调用 */
    double t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        rkvc_err err = rkvc_upscale_nv12(src, dst, SRC_W, SRC_H,
                                         DST_W, DST_H, RKVC_UPSCALE_BILINEAR);
        if (err != RKVC_OK) {
            fprintf(stderr, "upscale_nv12: %s（RGA 不可用？）\n", rkvc_err_str(err));
            free(src);
            free(dst);
            return 1;
        }
    }
    double one_shot_ms = now_ms() - t0;

    /* ctx API：import 一次，逐帧 memcpy 进内部缓冲再 process */
    rkvc_upscale_ctx *ctx =
        rkvc_upscale_ctx_create(SRC_W, SRC_H, DST_W, DST_H, RKVC_UPSCALE_BILINEAR);
    if (!ctx) {
        fprintf(stderr, "upscale_ctx_create failed（RGA 不可用？）\n");
        free(src);
        free(dst);
        return 1;
    }

    t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        memcpy(rkvc_upscale_ctx_src_buf(ctx), src, rkvc_upscale_ctx_src_bytes(ctx));
        rkvc_err err = rkvc_upscale_ctx_process(ctx);
        if (err != RKVC_OK) {
            fprintf(stderr, "ctx_process: %s\n", rkvc_err_str(err));
            rkvc_upscale_ctx_destroy(ctx);
            free(src);
            free(dst);
            return 1;
        }
        memcpy(dst, rkvc_upscale_ctx_dst_buf(ctx), rkvc_upscale_ctx_dst_bytes(ctx));
    }
    double ctx_ms = now_ms() - t0;

    printf("upscale %dx%d -> %dx%d  %d frames: one-shot %.2f ms/f, ctx %.2f ms/f (%.1fx)\n",
           SRC_W, SRC_H, DST_W, DST_H, frames,
           one_shot_ms / frames, ctx_ms / frames, one_shot_ms / ctx_ms);

    rkvc_upscale_ctx_destroy(ctx);
    free(src);
    free(dst);
    return 0;
}
