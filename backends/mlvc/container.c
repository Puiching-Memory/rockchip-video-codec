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

static int32_t rd_i32(const uint8_t *p)
{
    return (int32_t)rd_u32(p);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void mlvc_container_write_header(uint8_t *out64,
                                 const mlvc_container_header *hdr)
{
    memset(out64, 0, MLVC_HDR_SIZE);
    memcpy(out64, MLVC_MAGIC, MLVC_MAGIC_LEN);
    out64[5] = MLVC_FORMAT;
    wr_u32(out64 + 8, hdr->width);
    wr_u32(out64 + 12, hdr->height);
    wr_u32(out64 + 16, hdr->fps_num ? hdr->fps_num : 30);
    wr_u32(out64 + 20, hdr->fps_den ? hdr->fps_den : 1);
    wr_u32(out64 + 24, hdr->qp);
    /* frame_count@28 保持 0（读端以 EOF 为准）*/
    wr_u32(out64 + 32, hdr->iframe_period);
    wr_u32(out64 + 36, hdr->ltr_start_idx);
    wr_u32(out64 + 40, hdr->ltr_period);
    wr_u32(out64 + 44, hdr->flags);
    wr_u32(out64 + 48, hdr->target_bitrate_bps);
}

int mlvc_container_parse_header(const uint8_t *in, size_t size,
                                mlvc_container_header *out)
{
    if (!in || !out)
        return -1;
    if (size < MLVC_HDR_SIZE)
        return 0;
    if (memcmp(in, MLVC_MAGIC, MLVC_MAGIC_LEN) != 0)
        return -1;
    if (in[5] != MLVC_FORMAT)
        return -1;
    memset(out, 0, sizeof(*out));
    out->width = rd_u32(in + 8);
    out->height = rd_u32(in + 12);
    out->fps_num = rd_u32(in + 16);
    out->fps_den = rd_u32(in + 20);
    out->qp = rd_u32(in + 24);
    out->frame_count = rd_u32(in + 28);
    out->iframe_period = rd_u32(in + 32);
    out->ltr_start_idx = rd_u32(in + 36);
    out->ltr_period = rd_u32(in + 40);
    out->flags = rd_u32(in + 44);
    out->target_bitrate_bps = rd_u32(in + 48);
    if (!out->width || !out->height)
        return -1;
    return (int)MLVC_HDR_SIZE;
}

void mlvc_container_write_record(uint8_t *out16, uint32_t payload_size,
                                 int32_t q_index, uint32_t flags)
{
    wr_u32(out16, payload_size);
    wr_u32(out16 + 4, (uint32_t)q_index);
    wr_u32(out16 + 8, flags);
    wr_u32(out16 + 12, 0);
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
                    size_t *out_size, int32_t *out_q_index,
                    uint32_t *out_flags)
{
    uint32_t sz;

    if (!d || !out_data || !out_size || !out_q_index || !out_flags)
        return -1;

    if (!d->have_header) {
        int hdr_bytes = mlvc_container_parse_header(d->buf, d->len, &d->hdr);
        if (hdr_bytes == 0)
            return 0;
        if (hdr_bytes < 0) {
            d->parse_error = 1;
            return -1;
        }
        d->have_header = 1;
        memmove(d->buf, d->buf + hdr_bytes, d->len - (size_t)hdr_bytes);
        d->len -= (size_t)hdr_bytes;
    }

    if (d->len < MLVC_REC_SIZE)
        return 0;
    sz = rd_u32(d->buf);
    *out_q_index = rd_i32(d->buf + 4);
    *out_flags = rd_u32(d->buf + 8);
    if (*out_q_index == MLVC_QINDEX_DROPPED) {
        /* 丢帧标记帧：无载荷，恒完整 */
        if (sz != 0) {
            d->parse_error = 1;
            return -1;
        }
        *out_data = NULL;
        *out_size = 0;
        d->frames_emitted++;
        return 1;
    }
    if (sz == 0 || sz > MLVC_MAX_FRAME_BYTES) {
        d->parse_error = 1;
        return -1;
    }
    if (d->len < MLVC_REC_SIZE + (size_t)sz)
        return 0;

    *out_data = d->buf + MLVC_REC_SIZE;
    *out_size = sz;
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
