/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#ifndef RKVC_SHA256_H
#define RKVC_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct rkvc_sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buflen;
} rkvc_sha256;

void rkvc_sha256_init(rkvc_sha256 *s);
void rkvc_sha256_update(rkvc_sha256 *s, const void *data, size_t len);
void rkvc_sha256_final(rkvc_sha256 *s, uint8_t out[32]);

#endif /* RKVC_SHA256_H */
