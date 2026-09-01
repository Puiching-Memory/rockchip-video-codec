/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "example_common.h"

/* Send each synthetic NV12 frame through UDP loopback before job_push(). */
int main(int argc, char **argv) {
    const char *output = argc > 1 ? argv[1] : "loopback.h264";
    return example_stream_encode(output, RKVC_CODEC_H264, 160, 120, 90,
                                 0, 0, 1);
}
