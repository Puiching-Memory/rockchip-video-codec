/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file qptab.h
 * @brief MLVC QPT1 逐 QP FiLM 行表加载（布局与 tools/mlvc/qptab.py 一致）。
 *
 *   magic "QPT1"
 *   uint32 table_count
 *   每表：uint32 name_len, char name[name_len]（<=32，无 NUL）,
 *         uint32 rows, uint32 cols, uint16 data[rows*cols]（fp16 行主序）
 *
 * qp-dynamic 模型把每张 FiLM 条件表（q_encoder/q_decoder/q_feature/q_recon）
 * 从图内摘出，运行时按当前 q 取一行喂给对应 q_*_row 图输入。
 */

#ifndef RKVC_BACKEND_MLVC_QPTAB_H
#define RKVC_BACKEND_MLVC_QPTAB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MLVC_QPTAB_MAX_TABLES 8u
#define MLVC_QPTAB_NAME_MAX   32u

typedef struct {
    char      name[MLVC_QPTAB_NAME_MAX + 1]; /**< 图输入名，如 "q_encoder_row" */
    uint32_t  rows;                          /**< QP 档位行数（通常 72） */
    uint32_t  cols;                          /**< 通道数 */
    uint16_t *data;                          /**< rows*cols fp16 行主序（深拷贝） */
} mlvc_qptab_table;

typedef struct {
    mlvc_qptab_table tables[MLVC_QPTAB_MAX_TABLES];
    uint32_t         count;
} mlvc_qptab;

/**
 * @brief 从 .rkmodel 载荷字节解析 QPT1 表集合（数据深拷贝，节点自持）。
 * @return 0 成功；负 rkvc_status 失败并清零结构。
 */
int mlvc_qptab_load(const unsigned char *data, size_t size, mlvc_qptab *q);

/** 按图输入名查表，未命中返回 NULL。 */
const mlvc_qptab_table *mlvc_qptab_find(const mlvc_qptab *q, const char *name);

/** 取第 q 行（q >= rows 时钳到末行），返回 cols 个 fp16。 */
const uint16_t *mlvc_qptab_row(const mlvc_qptab_table *t, uint32_t q);

void mlvc_qptab_free(mlvc_qptab *q);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_QPTAB_H */
