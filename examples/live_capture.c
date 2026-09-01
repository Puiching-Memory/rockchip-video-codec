/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "example_common.h"

/* A synthetic capture source; replace frame generation with a camera callback. */
int main(int argc, char **argv) {
    const char *output = argc > 1 ? argv[1] : "live.h264";
    return example_stream_encode(output, RKVC_CODEC_H264, 640, 480, 300,
                                 0, 0, 0);
}
