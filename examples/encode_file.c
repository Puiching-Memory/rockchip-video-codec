/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "example_common.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s input.nv12 output.h264 width height\n", argv[0]);
        return 2;
    }
    return example_run_file(RKVC_OPERATION_ENCODE, RKVC_CODEC_H264,
                            argv[1], argv[2], (uint32_t)strtoul(argv[3], NULL, 10),
                            (uint32_t)strtoul(argv[4], NULL, 10), 2000000);
}
