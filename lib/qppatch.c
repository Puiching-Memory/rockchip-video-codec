/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "qppatch.h"
#include "internal.h"
#include "platform.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <zlib.h>

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

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uLong c = crc32(0L, Z_NULL, 0);
    while (n) {
        uInt chunk = n > 0x40000000u ? 0x40000000u : (uInt)n;
        c = crc32(c, p, chunk);
        p += chunk;
        n -= chunk;
    }
    return (uint32_t)c;
}

rkvc_err rkvc_qppatch_build_path(char *buf, size_t cap, const char *dir,
                                 const char *part, int qp)
{
    if (!buf || cap < 8 || !dir || !dir[0] || !part || !part[0] || qp < 0)
        return RKVC_ERR_INVALID;
    size_t n = strlen(dir);
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
        n--;
    int wr = snprintf(buf, cap, "%.*s/%s_qp%d.qppatch", (int)n, dir, part, qp);
    if (wr < 0 || (size_t)wr >= cap)
        return RKVC_ERR_INVALID;
    return RKVC_OK;
}

rkvc_err rkvc_qppatch_resolve(const char *dir, const char *part, int qp,
                              char *buf, size_t cap, const char **out_path)
{
    if (!out_path || !buf)
        return RKVC_ERR_INVALID;
    *out_path = NULL;
    if (!dir || !dir[0])
        return RKVC_OK;
    const rkvc_platform_info *platform = rkvc_platform_probe();
    if (platform->soc[0] != '\0') {
        size_t n = strlen(dir);
        while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
            n--;
        int wr = snprintf(buf, cap, "%.*s/%s/%s_qp%d.qppatch",
                          (int)n, dir, platform->soc, part, qp);
        if (wr < 0 || (size_t)wr >= cap)
            return RKVC_ERR_INVALID;
        if (access(buf, R_OK) == 0) {
            *out_path = buf;
            return RKVC_OK;
        }
    }
    /* 兼容旧平铺目录，也允许调用方直接传 qp_patches/<soc>。 */
    rkvc_err err = rkvc_qppatch_build_path(buf, cap, dir, part, qp);
    if (err)
        return err;
    if (access(buf, R_OK) != 0) {
        RKVC_LOG("MLVC qp patch missing: %s", buf);
        return RKVC_ERR_IO;
    }
    *out_path = buf;
    return RKVC_OK;
}

rkvc_err rkvc_qppatch_apply(uint8_t *base, size_t base_size,
                            const uint8_t *patch, size_t patch_size,
                            int expected_qp)
{
    if (!base || !patch)
        return RKVC_ERR_INVALID;
    if (patch_size < RKVC_QPPATCH_HEADER_SIZE)
        return RKVC_ERR_FORMAT;
    if (memcmp(patch, RKVC_QPPATCH_MAGIC, 4) != 0)
        return RKVC_ERR_FORMAT;

    uint32_t version = rd_u32le(patch + 4);
    if (version != RKVC_QPPATCH_VERSION)
        return RKVC_ERR_FORMAT;

    uint64_t declared = rd_u64le(patch + 8);
    if (declared != (uint64_t)base_size)
        return RKVC_ERR_FORMAT;

    uint32_t qp = rd_u32le(patch + 16);
    uint32_t num_ranges = rd_u32le(patch + 20);
    uint32_t flags = rd_u32le(patch + 24);
    uint32_t base_crc = rd_u32le(patch + 28);
    uint32_t payload_crc = rd_u32le(patch + 32);
    (void)rd_u32le(patch + 36); /* coalesce_gap，仅记录 */
    if (flags != 0)
        return RKVC_ERR_FORMAT;
    if (expected_qp >= 0 && qp != (uint32_t)expected_qp) {
        RKVC_LOG("MLVC qp patch qp=%u, expected %d", qp, expected_qp);
        return RKVC_ERR_FORMAT;
    }

    if (crc32_buf(base, base_size) != base_crc) {
        RKVC_LOG("MLVC qp patch base crc mismatch");
        return RKVC_ERR_FORMAT;
    }

#if SIZE_MAX < UINT64_MAX
    if (num_ranges >
        (SIZE_MAX - RKVC_QPPATCH_HEADER_SIZE) / RKVC_QPPATCH_RANGE_SIZE)
        return RKVC_ERR_FORMAT;
#endif
    size_t ranges_bytes = (size_t)num_ranges * RKVC_QPPATCH_RANGE_SIZE;
    if (patch_size < RKVC_QPPATCH_HEADER_SIZE + ranges_bytes)
        return RKVC_ERR_FORMAT;

    const uint8_t *ranges = patch + RKVC_QPPATCH_HEADER_SIZE;
    const uint8_t *payload = ranges + ranges_bytes;
    size_t payload_size = patch_size - (RKVC_QPPATCH_HEADER_SIZE + ranges_bytes);

    uint32_t i;
    size_t need = 0;
    for (i = 0; i < num_ranges; i++) {
        const uint8_t *r = ranges + (size_t)i * RKVC_QPPATCH_RANGE_SIZE;
        uint32_t off = rd_u32le(r);
        uint32_t len = rd_u32le(r + 4);
        if ((uint64_t)off + (uint64_t)len > (uint64_t)base_size)
            return RKVC_ERR_FORMAT;
        if (len > SIZE_MAX - need)
            return RKVC_ERR_FORMAT;
        need += len;
    }
    if (need != payload_size)
        return RKVC_ERR_FORMAT;
    if (crc32_buf(payload, payload_size) != payload_crc)
        return RKVC_ERR_FORMAT;

    size_t cursor = 0;
    for (i = 0; i < num_ranges; i++) {
        const uint8_t *r = ranges + (size_t)i * RKVC_QPPATCH_RANGE_SIZE;
        uint32_t off = rd_u32le(r);
        uint32_t len = rd_u32le(r + 4);
        if (len)
            memcpy(base + off, payload + cursor, len);
        cursor += len;
    }
    return RKVC_OK;
}

rkvc_err rkvc_qppatch_apply_file(uint8_t *base, size_t base_size,
                                 const char *patch_path, int expected_qp)
{
    if (!patch_path || !patch_path[0])
        return RKVC_ERR_INVALID;
    FILE *fp = fopen(patch_path, "rb");
    if (!fp)
        return RKVC_ERR_IO;
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    long sz = ftell(fp);
    if (sz < 0 || (size_t)sz < RKVC_QPPATCH_HEADER_SIZE) {
        fclose(fp);
        return RKVC_ERR_FORMAT;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return RKVC_ERR_IO;
    }
    uint8_t *blob = rkvc_malloc((size_t)sz);
    if (!blob) {
        fclose(fp);
        return RKVC_ERR_NOMEM;
    }
    if (fread(blob, 1, (size_t)sz, fp) != (size_t)sz) {
        fclose(fp);
        rkvc_free(blob);
        return RKVC_ERR_IO;
    }
    fclose(fp);
    rkvc_err err = rkvc_qppatch_apply(base, base_size, blob, (size_t)sz, expected_qp);
    rkvc_free(blob);
    return err;
}
