/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** psnr_test.c — v2 占位（完整 PSNR 需参考帧对） */
#include "rkvc/rkvc.h"
#include <stdio.h>
int main(void) {
    printf("psnr_test: use transcode + external metric tools in v2\n");
    rkvc_init();
    rkvc_caps c;
    rkvc_query_caps(&c);
    printf("codecs: h264=%d hevc=%d av1=%d\n",
           c.has_h264_dec, c.has_hevc_dec, c.has_av1_dec);
    return 0;
}
