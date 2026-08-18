/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file live_capture.c
 * @brief V4L2 采集编码最小示例；`mock` 设备为合成 NV12 源，无摄像头也能跑。
 *
 * 用法: example_live_capture [设备|mock] [out.mp4] [帧数] [WxH]
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "mock";
    const char *out = argc > 2 ? argv[2] : "/tmp/rkvc_live.mp4";
    int frames = argc > 3 ? atoi(argv[3]) : 90;
    int w = 640, h = 480;
    if (argc > 4) {
        int tw = 0, th = 0;
        char extra;
        if (sscanf(argv[4], "%dx%d%c", &tw, &th, &extra) != 2 || tw <= 0 || th <= 0) {
            fprintf(stderr, "invalid size: %s (expected WxH)\n", argv[4]);
            return 1;
        }
        w = tw;
        h = th;
    }

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d);
    d.capture_device = dev;
    d.output_path = out;
    d.width = w;
    d.height = h;
    d.capture_max_frames = frames;
    d.bitrate = 2000000;
    d.policy = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "create: %s\n", rkvc_err_str(err));
        return 1;
    }

    err = rkvc_session_run_file(s);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("capture %s -> %s frames_in=%llu frames_out=%llu avg_fps=%.2f err=%s\n",
           dev, out, (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out, st.avg_fps, rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
