/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

#include "container.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void mlvc_container_write_header(uint8_t *out32,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps_num, uint32_t fps_den,
                                 uint32_t qp)
{
    memset(out32, 0, MLVC_HDR_SIZE);
    memcpy(out32, MLVC_MAGIC, MLVC_MAGIC_LEN);
    out32[5] = MLVC_VERSION;
    wr_u32(out32 + 8, width);
    wr_u32(out32 + 12, height);
    wr_u32(out32 + 16, fps_num ? fps_num : 30);
    wr_u32(out32 + 20, fps_den ? fps_den : 1);
    wr_u32(out32 + 24, qp);
    /* frame_count@28 保持 0（见 container.h：读端以 EOF 为准）*/
}

int mlvc_container_parse_header(const uint8_t *in32,
                                mlvc_container_header *out)
{
    if (!in32 || !out)
        return -1;
    if (memcmp(in32, MLVC_MAGIC, MLVC_MAGIC_LEN) != 0 ||
        in32[5] != MLVC_VERSION)
        return -1;
    out->width = rd_u32(in32 + 8);
    out->height = rd_u32(in32 + 12);
    out->fps_num = rd_u32(in32 + 16);
    out->fps_den = rd_u32(in32 + 20);
    out->qp = rd_u32(in32 + 24);
    out->frame_count = rd_u32(in32 + 28);
    if (!out->width || !out->height)
        return -1;
    return 0;
}

void mlvc_container_write_record(uint8_t *out8, uint32_t payload_size,
                                 int keyframe)
{
    wr_u32(out8, payload_size);
    out8[4] = keyframe ? 1 : 0;
    out8[5] = 0;
    out8[6] = 0;
    out8[7] = 0;
}

void mlvc_demux_init(mlvc_demuxer *d)
{
    memset(d, 0, sizeof(*d));
}

void mlvc_demux_free(mlvc_demuxer *d)
{
    if (!d)
        return;
    free(d->buf);
    d->buf = NULL;
    d->len = d->cap = 0;
}

int mlvc_demux_append(mlvc_demuxer *d, const uint8_t *data, size_t size)
{
    if (!d || (!data && size))
        return -2; /* INVALID */
    if (d->parse_error)
        return -8; /* FORMAT */
    if (d->len + size > d->cap) {
        size_t ncap = d->cap ? d->cap * 2 : 64 * 1024;
        uint8_t *nbuf;
        while (ncap < d->len + size)
            ncap *= 2;
        nbuf = realloc(d->buf, ncap);
        if (!nbuf)
            return -1; /* NOMEM */
        d->buf = nbuf;
        d->cap = ncap;
    }
    memcpy(d->buf + d->len, data, size);
    d->len += size;
    return 0;
}

size_t mlvc_demux_consumed(const mlvc_demuxer *d)
{
    return d ? d->len : 0;
}

int mlvc_demux_next(mlvc_demuxer *d, const uint8_t **out_data,
                    size_t *out_size, int *out_keyframe)
{
    if (!d || !out_data || !out_size || !out_keyframe)
        return -1;

    if (!d->have_header) {
        if (d->len < MLVC_HDR_SIZE)
            return 0;
        if (mlvc_container_parse_header(d->buf, &d->hdr) != 0) {
            d->parse_error = 1;
            return -1;
        }
        d->have_header = 1;
        memmove(d->buf, d->buf + MLVC_HDR_SIZE, d->len - MLVC_HDR_SIZE);
        d->len -= MLVC_HDR_SIZE;
    }

    if (d->len < MLVC_REC_SIZE)
        return 0;
    uint32_t sz = rd_u32(d->buf);
    int kf = d->buf[4] ? 1 : 0;
    if (sz == 0 || sz > MLVC_MAX_FRAME_BYTES) {
        d->parse_error = 1;
        return -1;
    }
    if (d->len < MLVC_REC_SIZE + (size_t)sz)
        return 0;

    *out_data = d->buf + MLVC_REC_SIZE;
    *out_size = sz;
    *out_keyframe = kf;
    d->frames_emitted++;
    return 1;
}

void mlvc_demux_consume(mlvc_demuxer *d, size_t bytes)
{
    if (!d || bytes > d->len)
        return;
    memmove(d->buf, d->buf + bytes, d->len - bytes);
    d->len -= bytes;
}
