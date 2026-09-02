/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "qppatch.h"

#include "rkvc/api.h"

#include <string.h>

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64le(const uint8_t *p)
{
    return (uint64_t)rd_u32le(p) | ((uint64_t)rd_u32le(p + 4) << 32);
}

/* 内嵌 CRC-32（IEEE 802.3，与 zlib crc32 同多项式），
 * 去掉 DSO 对 zlib 的链接依赖。 */
static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static int table_ready;
    uint32_t c = 0xFFFFFFFFu;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i;
            for (int k = 0; k < 8; ++k)
                r = (r >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(r & 1)));
            table[i] = r;
        }
        table_ready = 1;
    }
    while (n--) {
        c = table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

int mlvc_qppatch_apply(uint8_t *base, size_t base_size,
                       const uint8_t *patch, size_t patch_size,
                       int expected_qp)
{
    if (!base || !patch)
        return (int)RKVC_STATUS_INVALID;
    if (patch_size < MLVC_QPPATCH_HEADER_SIZE)
        return (int)RKVC_STATUS_FORMAT;
    if (memcmp(patch, MLVC_QPPATCH_MAGIC, 4) != 0)
        return (int)RKVC_STATUS_FORMAT;

    uint32_t version = rd_u32le(patch + 4);
    if (version != MLVC_QPPATCH_VERSION)
        return (int)RKVC_STATUS_FORMAT;

    uint64_t declared = rd_u64le(patch + 8);
    if (declared != (uint64_t)base_size)
        return (int)RKVC_STATUS_FORMAT;

    uint32_t qp = rd_u32le(patch + 16);
    uint32_t num_ranges = rd_u32le(patch + 20);
    uint32_t flags = rd_u32le(patch + 24);
    uint32_t base_crc = rd_u32le(patch + 28);
    uint32_t payload_crc = rd_u32le(patch + 32);
    (void)rd_u32le(patch + 36); /* coalesce_gap，仅记录 */
    if (flags != 0)
        return (int)RKVC_STATUS_FORMAT;
    if (expected_qp >= 0 && qp != (uint32_t)expected_qp)
        return (int)RKVC_STATUS_FORMAT;

    if (crc32_buf(base, base_size) != base_crc)
        return (int)RKVC_STATUS_FORMAT;

    if (num_ranges >
        (UINT32_MAX - MLVC_QPPATCH_HEADER_SIZE) / MLVC_QPPATCH_RANGE_SIZE)
        return (int)RKVC_STATUS_FORMAT;
    size_t ranges_bytes = (size_t)num_ranges * MLVC_QPPATCH_RANGE_SIZE;
    if (patch_size < MLVC_QPPATCH_HEADER_SIZE + ranges_bytes)
        return (int)RKVC_STATUS_FORMAT;

    const uint8_t *ranges = patch + MLVC_QPPATCH_HEADER_SIZE;
    const uint8_t *payload = ranges + ranges_bytes;
    size_t payload_size = patch_size - (MLVC_QPPATCH_HEADER_SIZE + ranges_bytes);

    uint32_t i;
    size_t need = 0;
    for (i = 0; i < num_ranges; i++) {
        const uint8_t *r = ranges + (size_t)i * MLVC_QPPATCH_RANGE_SIZE;
        uint32_t off = rd_u32le(r);
        uint32_t len = rd_u32le(r + 4);
        if ((uint64_t)off + (uint64_t)len > (uint64_t)base_size)
            return (int)RKVC_STATUS_FORMAT;
        if (len > SIZE_MAX - need)
            return (int)RKVC_STATUS_FORMAT;
        need += len;
    }
    if (need != payload_size)
        return (int)RKVC_STATUS_FORMAT;
    if (crc32_buf(payload, payload_size) != payload_crc)
        return (int)RKVC_STATUS_FORMAT;

    size_t cursor = 0;
    for (i = 0; i < num_ranges; i++) {
        const uint8_t *r = ranges + (size_t)i * MLVC_QPPATCH_RANGE_SIZE;
        uint32_t off = rd_u32le(r);
        uint32_t len = rd_u32le(r + 4);
        if (len)
            memcpy(base + off, payload + cursor, len);
        cursor += len;
    }
    return (int)RKVC_STATUS_OK;
}
