/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_fixtures.c
 * @brief 测试夹具生成（Session 编码，无外部样本文件）。
 */

#include "test_fixtures.h"

#include "rkvc/rkvc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int rkvc_test_write_nv12_pattern(const char *path, int w, int h, int frames)
{
    if (!path || w <= 0 || h <= 0 || frames <= 0)
        return -1;

    FILE *fp = fopen(path, "wb");
    if (!fp)
        return -1;

    const size_t y_plane = (size_t)w * (size_t)h;
    const size_t frame_sz = y_plane + y_plane / 2;
    uint8_t *frame = malloc(frame_sz);
    if (!frame) {
        fclose(fp);
        return -1;
    }

    for (int i = 0; i < frames; i++) {
        memset(frame, (uint8_t)((i * 17) & 0xff), y_plane);
        memset(frame + y_plane, 128, frame_sz - y_plane);
        if (fwrite(frame, 1, frame_sz, fp) != frame_sz) {
            free(frame);
            fclose(fp);
            return -1;
        }
    }

    free(frame);
    fclose(fp);
    return 0;
}

static int encode_nv12_to_mp4(const char *nv12, const char *mp4,
                              int w, int h, int fps_num)
{
    rkvc_init();

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path  = nv12;
    d.output_path = mp4;
    d.width       = w;
    d.height      = h;
    d.fps_num     = fps_num > 0 ? fps_num : 30;
    d.fps_den     = 1;
    d.bitrate     = 1000000;
    d.policy      = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    if (rkvc_session_create(&d, &s) != RKVC_OK)
        return -1;
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : -1;
}

int rkvc_test_ensure_h264_mp4(char *path, size_t path_sz,
                              int w, int h, int frames)
{
    if (!path || path_sz < 32 || w <= 0 || h <= 0 || frames <= 0)
        return -1;

    if (path[0] && access(path, R_OK) == 0)
        return 0;

    char raw[PATH_MAX];
    snprintf(raw, sizeof(raw), "/tmp/rkvc_fixture_%dx%d_%d.raw.nv12", w, h, frames);
    if (rkvc_test_write_nv12_pattern(raw, w, h, frames) != 0)
        return -1;

    snprintf(path, path_sz, "/tmp/rkvc_fixture_%dx%d_%d.mp4", w, h, frames);
    if (encode_nv12_to_mp4(raw, path, w, h, 30) != 0) {
        unlink(raw);
        return -1;
    }
    unlink(raw);
    return 0;
}

const char *rkvc_test_fixture_h264_mp4(int w, int h, int frames)
{
    static char path[PATH_MAX];
    if (rkvc_test_ensure_h264_mp4(path, sizeof(path), w, h, frames) != 0)
        return NULL;
    return path;
}

const char *rkvc_test_fixture_nv12_1080p(void)
{
    static char path[PATH_MAX];
    static int ready;

    if (ready && access(path, R_OK) == 0)
        return path;

    snprintf(path, sizeof(path), "/tmp/rkvc_fixture_1920x1080_5.nv12");
    if (access(path, R_OK) != 0) {
        if (rkvc_test_write_nv12_pattern(path, 1920, 1080, 5) != 0)
            return NULL;
    }
    ready = 1;
    return path;
}
