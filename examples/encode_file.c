/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file encode_file.c
 * @brief 示例：端口 push 编码 NV12 测试图案。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>

static int w = 1920, h = 1080, fps = 30, frames = 300;
static int64_t bitrate = 4000000;

int main(int argc, char **argv)
{
    const char *output = "output.mp4";
    int c;
    static struct option opts[] = {
        {"output", required_argument, 0, 'o'},
        {"size", required_argument, 0, 's'},
        {"rate", required_argument, 0, 'r'},
        {"frames", required_argument, 0, 'n'},
        {"bitrate", required_argument, 0, 'b'},
        {0, 0, 0, 0}
    };
    while ((c = getopt_long(argc, argv, "o:s:r:n:b:", opts, NULL)) != -1) {
        if (c == 'o') output = optarg;
        else if (c == 's') sscanf(optarg, "%dx%d", &w, &h);
        else if (c == 'r') fps = atoi(optarg);
        else if (c == 'n') frames = atoi(optarg);
        else if (c == 'b') bitrate = atoll(optarg);
    }

    char raw_path[] = "/tmp/rkvc_example_raw_XXXXXX";
    int fd = mkstemp(raw_path);
    if (fd < 0)
        return 1;

    size_t frame_sz = (size_t)w * (size_t)h * 3 / 2;
    uint8_t *raw = malloc(frame_sz);
    for (int i = 0; i < frames; i++) {
        memset(raw, (uint8_t)(i & 0xff), (size_t)w * (size_t)h);
        memset(raw + (size_t)w * (size_t)h, 128, frame_sz - (size_t)w * (size_t)h);
        if (write(fd, raw, frame_sz) != (ssize_t)frame_sz) {
            close(fd);
            free(raw);
            return 1;
        }
    }
    close(fd);
    free(raw);

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_ENCODE, &d);
    d.input_path  = raw_path;
    d.output_path = output;
    d.width = w;
    d.height = h;
    d.fps_num = fps;
    d.bitrate = bitrate;
    d.policy  = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_session_create(&d, &s);
    rkvc_err err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    unlink(raw_path);
    return err == RKVC_OK ? 0 : 1;
}
