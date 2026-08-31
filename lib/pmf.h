/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file pmf.h
 * @brief MLVC PMF1 二进制表加载（布局与 tools/mlvc/pmf.py 一致，供跨语言契约测试）。
 */

#ifndef RKVC_PMF_H
#define RKVC_PMF_H

#include "rkvc/types.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int32_t *lengths;
    int32_t *offsets;
    int32_t *table;
    uint32_t num_lengths;
    uint32_t num_offsets;
    uint32_t num_table;
    double scale_min;
    double scale_max;
    uint32_t scale_levels;
    uint32_t index_space;
    uint32_t qp_num;
    uint32_t channels;
} rkvc_pmf;

/** 从 PMF1 文件加载（magic "PMF1" + nL/nO/nT + 数组 + tag 载荷）。 */
rkvc_err rkvc_pmf_load(rkvc_pmf *p, const char *path);

/** 释放内部数组并清零结构。 */
void rkvc_pmf_free(rkvc_pmf *p);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_PMF_H */
