/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** stream_device_pair.c — LIVE_CAPTURE 短录冒烟 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *dev = getenv("RKVC_V4L2_DEVICE");
    if (!dev || !dev[0])
        dev = "/dev/video-camera0";

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d);
    d.capture_device = dev;
    d.output_path = "/tmp/rkvc_stream_pair.mp4";
    d.width = 640;
    d.height = 480;
    d.capture_max_frames = 15;
    d.bitrate = 1500000;

    printf("stream_device_pair: LIVE_CAPTURE %s -> %s\n",
           d.capture_device, d.output_path);

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "create failed: %s (set RKVC_V4L2_DEVICE?)\n",
                rkvc_err_str(err));
        return 1;
    }
    err = rkvc_session_run_file(s);
    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("frames_in=%llu frames_out=%llu avg_fps=%.2f err=%s\n",
           (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out, st.avg_fps, rkvc_err_str(err));
    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
