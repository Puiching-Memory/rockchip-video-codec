/**
 * @file rkvc_yuv_upscale.c
 * @brief NV12 / YUV420p 文件批量缩放（RGA 硬件，供 bench 等调用）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "rkvc/rkvc.h"

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s --in IN --out OUT --sw W --sh H --dw W --dh H "
            "--algo nearest|bilinear|bicubic "
            "[--pix-fmt nv12|yuv420p] [--frames N]\n",
            prog);
}

static int upscale_nv12_file(int fin, int fout, int sw, int sh, int dw, int dh,
                             rkvc_upscale_algo algo, int frames)
{
    rkvc_upscale_ctx *ctx = rkvc_upscale_ctx_create(sw, sh, dw, dh, algo);
    if (!ctx) {
        fprintf(stderr, "rkvc_upscale_ctx_create failed\n");
        return 1;
    }

    const size_t src_frame = rkvc_upscale_ctx_src_bytes(ctx);
    const size_t dst_frame = rkvc_upscale_ctx_dst_bytes(ctx);
    uint8_t *src = rkvc_upscale_ctx_src_buf(ctx);
    uint8_t *dst = rkvc_upscale_ctx_dst_buf(ctx);

    for (int n = 0; n < frames; n++) {
        ssize_t nr = pread(fin, src, src_frame, (off_t)n * (off_t)src_frame);
        if (nr != (ssize_t)src_frame)
            break;

        if (rkvc_upscale_ctx_process(ctx) != RKVC_OK) {
            fprintf(stderr, "rkvc upscale failed on frame %d\n", n);
            rkvc_upscale_ctx_destroy(ctx);
            return 1;
        }

        ssize_t nw = pwrite(fout, dst, dst_frame, (off_t)n * (off_t)dst_frame);
        if (nw != (ssize_t)dst_frame) {
            perror("pwrite");
            rkvc_upscale_ctx_destroy(ctx);
            return 1;
        }
    }

    rkvc_upscale_ctx_destroy(ctx);
    return 0;
}

static int upscale_yuv420p_file(int fin, int fout, int sw, int sh, int dw, int dh,
                                rkvc_upscale_algo algo, int frames)
{
    const size_t src_frame = (size_t)sw * (size_t)sh * 3 / 2;
    const size_t dst_frame = (size_t)dw * (size_t)dh * 3 / 2;
    uint8_t *src_buf = malloc(src_frame);
    uint8_t *dst_buf = malloc(dst_frame);
    if (!src_buf || !dst_buf) {
        free(src_buf);
        free(dst_buf);
        return 1;
    }

    for (int n = 0; n < frames; n++) {
        ssize_t nr = pread(fin, src_buf, src_frame, (off_t)n * (off_t)src_frame);
        if (nr != (ssize_t)src_frame)
            break;

        rkvc_err err = rkvc_upscale_yuv420p(src_buf, dst_buf, sw, sh, dw, dh, algo);
        if (err != RKVC_OK) {
            fprintf(stderr, "rkvc upscale failed on frame %d\n", n);
            free(src_buf);
            free(dst_buf);
            return 1;
        }

        ssize_t nw = pwrite(fout, dst_buf, dst_frame, (off_t)n * (off_t)dst_frame);
        if (nw != (ssize_t)dst_frame) {
            perror("pwrite");
            free(src_buf);
            free(dst_buf);
            return 1;
        }
    }

    free(src_buf);
    free(dst_buf);
    return 0;
}

int main(int argc, char **argv)
{
    const char *in_path = NULL, *out_path = NULL, *algo_name = "bilinear";
    const char *pix_fmt = "nv12";
    int sw = 0, sh = 0, dw = 0, dh = 0, frames = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--in") && i + 1 < argc)
            in_path = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_path = argv[++i];
        else if (!strcmp(argv[i], "--sw") && i + 1 < argc)
            sw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sh") && i + 1 < argc)
            sh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dw") && i + 1 < argc)
            dw = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dh") && i + 1 < argc)
            dh = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--algo") && i + 1 < argc)
            algo_name = argv[++i];
        else if (!strcmp(argv[i], "--pix-fmt") && i + 1 < argc)
            pix_fmt = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc)
            frames = atoi(argv[++i]);
        else {
            usage(argv[0]);
            return 1;
        }
    }

    if (!in_path || !out_path || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        usage(argv[0]);
        return 1;
    }

    const int use_nv12 = (strcmp(pix_fmt, "nv12") == 0);
    const int use_yuv420p = (strcmp(pix_fmt, "yuv420p") == 0);
    if (!use_nv12 && !use_yuv420p) {
        fprintf(stderr, "unknown pix-fmt: %s\n", pix_fmt);
        return 1;
    }

    rkvc_upscale_algo algo;
    if (rkvc_upscale_algo_from_name(algo_name, &algo) != 0) {
        fprintf(stderr, "unknown algo: %s\n", algo_name);
        return 1;
    }

    rkvc_caps caps;
    if (rkvc_query_caps(&caps) != RKVC_OK || !caps.has_rga) {
        fprintf(stderr, "RGA not available\n");
        return 1;
    }

    const size_t src_frame = (size_t)sw * (size_t)sh * 3 / 2;
    int fin = open(in_path, O_RDONLY);
    if (fin < 0) {
        perror(in_path);
        return 1;
    }

    off_t in_end = lseek(fin, 0, SEEK_END);
    if (in_end < 0) {
        perror("lseek");
        close(fin);
        return 1;
    }
    lseek(fin, 0, SEEK_SET);

    int max_frames = (int)(in_end / (off_t)src_frame);
    if (max_frames <= 0) {
        fprintf(stderr, "input too small for one frame\n");
        close(fin);
        return 1;
    }
    if (frames <= 0 || frames > max_frames)
        frames = max_frames;

    const size_t dst_frame = (size_t)dw * (size_t)dh * 3 / 2;
    int fout = open(out_path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fout < 0) {
        perror(out_path);
        close(fin);
        return 1;
    }
    if (ftruncate(fout, (off_t)dst_frame * (size_t)frames) != 0) {
        perror("ftruncate");
        close(fin);
        close(fout);
        return 1;
    }

    int rc;
    if (use_nv12)
        rc = upscale_nv12_file(fin, fout, sw, sh, dw, dh, algo, frames);
    else
        rc = upscale_yuv420p_file(fin, fout, sw, sh, dw, dh, algo, frames);

    close(fin);
    close(fout);
    return rc;
}
