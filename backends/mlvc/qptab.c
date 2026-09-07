/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * MLVC QPT1 逐 QP FiLM 行表加载（布局与 tools/mlvc/qptab.py 一致）。
 *
 * 载荷缓冲生命周期只在 bind/open 阶段保证，表数据深拷出由后端自持。
 */

#include "qptab.h"

#include "rkvc/api.h"

#include <stdlib.h>
#include <string.h>

/* rows*cols 上界 1M 元素（约 2MB fp16），挡住计数回绕后的越界。 */
#define MLVC_QPTAB_MAX_ELEMS (1u << 20)

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void mlvc_qptab_free(mlvc_qptab *q)
{
    uint32_t i;
    if (!q)
        return;
    for (i = 0; i < q->count; i++)
        free(q->tables[i].data);
    memset(q, 0, sizeof(*q));
}

int mlvc_qptab_load(const unsigned char *data, size_t size, mlvc_qptab *q)
{
    const unsigned char *cur = data;
    size_t left = size;
    uint32_t count, i;

    memset(q, 0, sizeof(*q));
    if (!data || size < 8 || memcmp(data, "QPT1", 4) != 0)
        return (int)RKVC_STATUS_FORMAT;
    cur += 4;
    left -= 4;

    count = rd_u32(cur);
    cur += 4;
    left -= 4;
    if (count == 0 || count > MLVC_QPTAB_MAX_TABLES)
        return (int)RKVC_STATUS_FORMAT;

    for (i = 0; i < count; i++) {
        mlvc_qptab_table *t = &q->tables[i];
        uint32_t name_len, rows, cols;
        size_t bytes;

        if (left < 4)
            goto format;
        name_len = rd_u32(cur);
        cur += 4;
        left -= 4;
        if (name_len == 0 || name_len > MLVC_QPTAB_NAME_MAX || left < name_len + 8)
            goto format;
        memcpy(t->name, cur, name_len);
        t->name[name_len] = '\0';
        cur += name_len;
        left -= name_len;

        rows = rd_u32(cur);
        cols = rd_u32(cur + 4);
        cur += 8;
        left -= 8;
        if (rows == 0 || cols == 0 ||
            (uint64_t)rows * cols > MLVC_QPTAB_MAX_ELEMS)
            goto format;
        bytes = (size_t)rows * cols * 2u;
        if (left < bytes)
            goto format;

        t->data = malloc(bytes);
        if (!t->data) {
            mlvc_qptab_free(q);
            return (int)RKVC_STATUS_NOMEM;
        }
        memcpy(t->data, cur, bytes);
        t->rows = rows;
        t->cols = cols;
        cur += bytes;
        left -= bytes;
    }
    if (left != 0)
        goto format;
    q->count = count;
    return 0;

format:
    mlvc_qptab_free(q);
    return (int)RKVC_STATUS_FORMAT;
}

const mlvc_qptab_table *mlvc_qptab_find(const mlvc_qptab *q, const char *name)
{
    uint32_t i;
    if (!q || !name)
        return NULL;
    for (i = 0; i < q->count; i++) {
        if (strcmp(q->tables[i].name, name) == 0)
            return &q->tables[i];
    }
    return NULL;
}

const uint16_t *mlvc_qptab_row(const mlvc_qptab_table *t, uint32_t q)
{
    if (!t || !t->data)
        return NULL;
    if (q >= t->rows)
        q = t->rows - 1u;
    return t->data + (size_t)q * t->cols;
}
