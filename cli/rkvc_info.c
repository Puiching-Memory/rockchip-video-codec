/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_info.c
 * @brief v2 硬件能力查询工具。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <getopt.h>

static void usage(void)
{
    printf("rkvc_info — RK3588 multi-codec 能力查询\n"
           "  -v, --version   版本\n"
           "  -j, --json      JSON 输出\n");
}

int main(int argc, char **argv)
{
    int show_version = 0, json = 0;
    static struct option opts[] = {
        {"version", no_argument, 0, 'v'},
        {"json", no_argument, 0, 'j'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    int c;
    while ((c = getopt_long(argc, argv, "vjh", opts, NULL)) != -1) {
        if (c == 'v') show_version = 1;
        else if (c == 'j') json = 1;
        else { usage(); return c == 'h' ? 0 : 1; }
    }

    if (show_version) {
        if (rkvc_init() != RKVC_OK)
            return 1;
        printf("rkvc %s (0x%06x)\n", rkvc_version(), rkvc_version_number());
        return 0;
    }

    rkvc_init();
    rkvc_caps caps;
    if (rkvc_query_caps(&caps) != RKVC_OK)
        return 1;

    if (json) {
        printf("{\"version\":\"%s\",\"h264_enc\":%s,\"hevc_enc\":%s,"
               "\"av1_enc\":%s,\"h264_dec\":%s,\"hevc_dec\":%s,"
               "\"av1_dec\":%s,\"dma_heap\":%s,\"rga\":%s,\"rknn\":%s,"
               "\"max_width\":%d,\"max_height\":%d}\n",
               rkvc_version(),
               caps.has_h264_enc ? "true" : "false",
               caps.has_hevc_enc ? "true" : "false",
               caps.has_av1_enc ? "true" : "false",
               caps.has_h264_dec ? "true" : "false",
               caps.has_hevc_dec ? "true" : "false",
               caps.has_av1_dec ? "true" : "false",
               caps.has_dma_heap ? "true" : "false",
               caps.has_rga ? "true" : "false",
               caps.has_rknn ? "true" : "false",
               caps.max_width, caps.max_height);
    } else {
        printf("rkvc %s\n", rkvc_version());
        printf("H.264 enc/dec: %d/%d\n", caps.has_h264_enc, caps.has_h264_dec);
        printf("HEVC  enc/dec: %d/%d\n", caps.has_hevc_enc, caps.has_hevc_dec);
        printf("AV1   enc/dec: %d/%d\n", caps.has_av1_enc, caps.has_av1_dec);
        printf("DMA heap: %d  RGA: %d  RKNN: %d  max %dx%d\n",
               caps.has_dma_heap, caps.has_rga, caps.has_rknn,
               caps.max_width, caps.max_height);
    }
    return 0;
}
