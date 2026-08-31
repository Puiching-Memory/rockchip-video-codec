/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * MLVC PMF1 二进制表加载（布局与 tools/mlvc/pmf.py 一致）：
 *
 *   magic "PMF1"
 *   uint32 nL, nO, nT
 *   int32  lengths[nL], offsets[nO], table[nT]
 *   tag=1  gaussian: double scale_min, scale_max; uint32 scale_levels, index_space
 *   tag=2  bitest:   uint32 qp_num, channels
 *
 * 从 lib/node_mlvc.c 抽出，使 Python 工具产物（tools/mlvc/pmf.py）与 C
 * 消费方可由同一解析代码在契约测试中互相校验。
 */

#include "pmf.h"
#include "internal.h"

#include <stdio.h>
#include <string.h>

/* lengths/offsets 上界 1M 项；table 上界 16M 项（约 64MB），挡住计数回绕后的 fread 堆溢出。 */
#define RKVC_PMF_MAX_LEN  (1u << 20)
#define RKVC_PMF_MAX_TAB  (16u << 20)

void rkvc_pmf_free(rkvc_pmf *p)
{
    if (!p)
        return;
    rkvc_free(p->lengths);
    rkvc_free(p->offsets);
    rkvc_free(p->table);
    memset(p, 0, sizeof(*p));
}

rkvc_err rkvc_pmf_load(rkvc_pmf *p, const char *path)
{
    FILE *f = NULL;
    rkvc_err err = RKVC_ERR_FORMAT;

    memset(p, 0, sizeof(*p));
    f = fopen(path, "rb");
    if (!f)
        return RKVC_ERR_IO;

    char magic[4];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "PMF1", 4) != 0)
        goto fail;

    uint32_t nL, nO, nT;
    if (fread(&nL, 4, 1, f) != 1 || fread(&nO, 4, 1, f) != 1 ||
        fread(&nT, 4, 1, f) != 1)
        goto fail;
    if (nL == 0 || nL > RKVC_PMF_MAX_LEN ||
        nO == 0 || nO > RKVC_PMF_MAX_LEN ||
        nT == 0 || nT > RKVC_PMF_MAX_TAB)
        goto fail;

    p->num_lengths = nL;
    p->num_offsets = nO;
    p->num_table = nT;
    p->lengths = rkvc_malloc((size_t)nL * 4u);
    p->offsets = rkvc_malloc((size_t)nO * 4u);
    p->table   = rkvc_malloc((size_t)nT * 4u);
    if (!p->lengths || !p->offsets || !p->table) {
        err = RKVC_ERR_NOMEM;
        goto fail;
    }
    if (fread(p->lengths, 4, nL, f) != nL ||
        fread(p->offsets, 4, nO, f) != nO ||
        fread(p->table, 4, nT, f) != nT)
        goto fail;

    uint32_t tag = 0;
    if (fread(&tag, 4, 1, f) != 1)
        goto fail;
    if (tag == 1) {
        if (fread(&p->scale_min, 8, 1, f) != 1 ||
            fread(&p->scale_max, 8, 1, f) != 1 ||
            fread(&p->scale_levels, 4, 1, f) != 1 ||
            fread(&p->index_space, 4, 1, f) != 1)
            goto fail;
    } else if (tag == 2) {
        if (fread(&p->qp_num, 4, 1, f) != 1 ||
            fread(&p->channels, 4, 1, f) != 1)
            goto fail;
    } else {
        goto fail;
    }
    fclose(f);
    return RKVC_OK;

fail:
    fclose(f);
    rkvc_pmf_free(p);
    return err;
}
