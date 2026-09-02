/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * MLVC PMF1 二进制表内存加载（布局与 tools/mlvc/pmf.py 一致）。
 *
 * 从 .rkmodel 载荷字节解析：bind_model 交付的载荷在图内生命周期有限，
 * 数组深拷出由节点自持。
 */

#include "pmf.h"

#include "rkvc/api.h"

#include <stdlib.h>
#include <string.h>

/* lengths/offsets 上界 1M 项；table 上界 16M 项（约 64MB），
 * 挡住计数回绕后的越界。 */
#define MLVC_PMF_MAX_LEN  (1u << 20)
#define MLVC_PMF_MAX_TAB  (16u << 20)

static uint32_t rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static double rd_f64(const unsigned char *p)
{
    uint64_t bits = (uint64_t)rd_u32(p) | ((uint64_t)rd_u32(p + 4) << 32);
    double out;
    memcpy(&out, &bits, 8);
    return out;
}

void mlvc_pmf_free(mlvc_pmf *p)
{
    if (!p)
        return;
    free(p->lengths);
    free(p->offsets);
    free(p->table);
    memset(p, 0, sizeof(*p));
}

int mlvc_pmf_load(const unsigned char *data, size_t size, mlvc_pmf *p)
{
    const unsigned char *cur = data;
    size_t left = size;
    uint32_t nL, nO, nT, tag;

    memset(p, 0, sizeof(*p));
    if (!data || size < 20 || memcmp(data, "PMF1", 4) != 0)
        return (int)RKVC_STATUS_FORMAT;
    cur += 4;
    left -= 4;

    nL = rd_u32(cur);
    nO = rd_u32(cur + 4);
    nT = rd_u32(cur + 8);
    cur += 12;
    left -= 12;
    if (nL == 0 || nL > MLVC_PMF_MAX_LEN ||
        nO == 0 || nO > MLVC_PMF_MAX_LEN ||
        nT == 0 || nT > MLVC_PMF_MAX_TAB)
        return (int)RKVC_STATUS_FORMAT;
    if (left < (size_t)(nL + nO + nT) * 4u + 4u)
        return (int)RKVC_STATUS_FORMAT;

    p->num_lengths = nL;
    p->num_offsets = nO;
    p->num_table = nT;
    p->lengths = malloc((size_t)nL * 4u);
    p->offsets = malloc((size_t)nO * 4u);
    p->table = malloc((size_t)nT * 4u);
    if (!p->lengths || !p->offsets || !p->table) {
        mlvc_pmf_free(p);
        return (int)RKVC_STATUS_NOMEM;
    }
    memcpy(p->lengths, cur, (size_t)nL * 4u);
    cur += (size_t)nL * 4u;
    memcpy(p->offsets, cur, (size_t)nO * 4u);
    cur += (size_t)nO * 4u;
    memcpy(p->table, cur, (size_t)nT * 4u);
    cur += (size_t)nT * 4u;

    tag = rd_u32(cur);
    cur += 4;
    if (tag == 1) {
        if (size < (size_t)(cur - data) + 24)
            return (int)RKVC_STATUS_FORMAT;
        p->scale_min = rd_f64(cur);
        p->scale_max = rd_f64(cur + 8);
        p->scale_levels = rd_u32(cur + 16);
        p->index_space = rd_u32(cur + 20);
    } else if (tag == 2) {
        if (size < (size_t)(cur - data) + 8)
            return (int)RKVC_STATUS_FORMAT;
        p->qp_num = rd_u32(cur);
        p->channels = rd_u32(cur + 4);
    } else {
        return (int)RKVC_STATUS_FORMAT;
    }
    return (int)RKVC_STATUS_OK;
}
