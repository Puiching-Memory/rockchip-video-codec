/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file roi_encode.c
 * @brief ROI 智能压缩：关键区域保质量、背景省码、局部强制帧内刷新。
 *
 * mock 合成源 640x480 → H.264。演示 rkvc_session_set_roi 三个矩形：
 * 中心 qp_offset=-6（更清晰）、左上 qp_offset=+10（省码）、
 * 右下 force_intra=1（该宏块行强制帧内，抗误码扩散）。
 * 仅 MPP H.264/HEVC 路径生效；SVT-AV1 忽略 ROI。
 *
 * 用法: example_roi_encode [out.mp4] [帧数]
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "/tmp/rkvc_roi.mp4";
    int frames = argc > 2 ? atoi(argv[2]) : 90;
    const int w = 640, h = 480;

    rkvc_pipeline_desc d;
    rkvc_pipeline_from_template(RKVC_TEMPLATE_LIVE_CAPTURE, &d);
    d.capture_device = "mock";
    d.output_path = out;
    d.width = w;
    d.height = h;
    d.capture_max_frames = frames;
    d.bitrate = 1500000;
    d.policy = RKVC_POLICY_REALTIME;

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "create: %s\n", rkvc_err_str(err));
        return 1;
    }

    rkvc_roi_rect rois[3] = {
        /* 中心画面：人脸/车牌等关键区域，多给码 */
        { .x = w / 4, .y = h / 4, .w = w / 2, .h = h / 2,
          .qp_offset = -6, .force_intra = 0 },
        /* 左上角背景：强压省码 */
        { .x = 0, .y = 0, .w = 160, .h = 128,
          .qp_offset = 10, .force_intra = 0 },
        /* 右下角：每帧强制帧内，限制误码扩散范围 */
        { .x = w - 160, .y = h - 128, .w = 160, .h = 128,
          .qp_offset = 0, .force_intra = 1 },
    };
    err = rkvc_session_set_roi(s, rois, 3);
    if (err != RKVC_OK) {
        fprintf(stderr, "set_roi: %s\n", rkvc_err_str(err));
        rkvc_session_destroy(s);
        return 1;
    }

    err = rkvc_session_run_file(s);

    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("roi_encode -> %s frames_out=%llu bytes_out=%llu err=%s\n",
           out, (unsigned long long)st.frames_out,
           (unsigned long long)st.bytes_out, rkvc_err_str(err));

    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
