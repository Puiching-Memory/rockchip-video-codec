/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file pmf.h
 * @brief MLVC PMF1 二进制表内存加载（布局与 tools/mlvc/pmf.py 一致）。
 *
 *   magic "PMF1"
 *   uint32 nL, nO, nT
 *   int32  lengths[nL], offsets[nO], table[nT]
 *   tag=1  gaussian: double scale_min, scale_max; uint32 scale_levels, index_space
 *   tag=2  bitest:   uint32 qp_num, channels
 */

#ifndef RKVC_BACKEND_MLVC_PMF_H
#define RKVC_BACKEND_MLVC_PMF_H

#include <stddef.h>
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
} mlvc_pmf;

/**
 * @brief 从 .rkmodel 载荷字节解析 PMF1 表。
 *
 * 内部数组深拷贝（载荷缓冲由图持有，生命周期只在 bind/open 阶段保证，
 * 拷出后节点自持）。失败返回负 rkvc_status 并清零结构。
 */
int mlvc_pmf_load(const unsigned char *data, size_t size, mlvc_pmf *p);

/** 释放内部数组并清零结构。 */
void mlvc_pmf_free(mlvc_pmf *p);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_PMF_H */
