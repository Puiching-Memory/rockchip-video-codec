/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "example_common.h"

int main(int argc, char **argv) {
    const char *output = argc > 1 ? argv[1] : "adaptive.h264";
    return example_stream_encode(output, RKVC_CODEC_H264, 640, 480, 120,
                                 0, 1, 0);
}
