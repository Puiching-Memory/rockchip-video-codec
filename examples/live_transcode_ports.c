/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "example_common.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s input.h264 output.h265\n", argv[0]);
        return 2;
    }
    return example_stream_transcode(argv[1], argv[2], RKVC_CODEC_HEVC);
}
