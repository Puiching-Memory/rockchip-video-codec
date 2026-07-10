/** live_capture.c — V4L2 LIVE_CAPTURE → H.264 短录 */
#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    const char *dev = argc > 1 ? argv[1] : "/dev/video-camera0";
    const char *out = argc > 2 ? argv[2] : "/tmp/rkvc_live.mp4";
    int frames = argc > 3 ? atoi(argv[3]) : 30;
    int w = 640, h = 480;
    if (argc > 4)
        sscanf(argv[4], "%dx%d", &w, &h);

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

    /* 中心 ROI：保留中间区域细节 */
    rkvc_roi_rect roi = {
        .x = w / 4, .y = h / 4, .w = w / 2, .h = h / 2,
        .qp_offset = -4, .force_intra = 0,
    };
    rkvc_session_set_roi(s, &roi, 1);

    /* 热切换示例：运行前也可改；运行中由应用层按带宽调用 */
    rkvc_session_set_bitrate(s, 1500000);
    rkvc_session_set_gop(s, 30);
    rkvc_session_request_idr(s);

    err = rkvc_session_run_file(s);
    rkvc_session_stats st;
    rkvc_session_get_stats(s, &st);
    printf("live_capture %s -> %s frames_in=%llu frames_out=%llu err=%s\n",
           dev, out, (unsigned long long)st.frames_in,
           (unsigned long long)st.frames_out, rkvc_err_str(err));
    rkvc_session_destroy(s);
    return err == RKVC_OK ? 0 : 1;
}
