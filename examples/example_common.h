/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef RKVC_EXAMPLE_COMMON_H
#define RKVC_EXAMPLE_COMMON_H

#include <stdint.h>

#include "rkvc/rkvc.h"

int example_run_file(rkvc_operation operation, rkvc_codec codec,
                     const char *input, const char *output,
                     uint32_t width, uint32_t height, int32_t bitrate);

int example_stream_encode(const char *output, rkvc_codec codec,
                          uint32_t width, uint32_t height, uint32_t frames,
                          int with_roi, int adaptive, int udp_loopback);

int example_stream_transcode(const char *input, const char *output,
                             rkvc_codec target_codec);

#endif
