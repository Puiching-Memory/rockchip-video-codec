/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file qppatch.h
 * @brief MLVC RKNN 多 QP 二进制补丁（QPP1）：对基座模型 memcpy 差异区间。
 *
 * 应用后字节流与该 QP 原生构建的 .rknn 逐字节相同。打开时打一次补丁，不做热切换。
 */

#ifndef RKVC_QPPATCH_H
#define RKVC_QPPATCH_H

#include "rkvc/types.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RKVC_QPPATCH_MAGIC        "QPP1"
#define RKVC_QPPATCH_VERSION      1u
#define RKVC_QPPATCH_HEADER_SIZE  48u
#define RKVC_QPPATCH_RANGE_SIZE   8u
#define RKVC_QPPATCH_COALESCE_GAP 64u

/**
 * 把 `dir/{part}_qp{qp}.qppatch` 写入 buf。
 * @param part `"enc"` 或 `"dec"`
 */
rkvc_err rkvc_qppatch_build_path(char *buf, size_t cap, const char *dir,
                                 const char *part, int qp);

/**
 * 若 dir 为空：*out_path = NULL。
 * 若 dir 非空：生成路径，文件不可读则返回 `RKVC_ERR_IO`。
 */
rkvc_err rkvc_qppatch_resolve(const char *dir, const char *part, int qp,
                              char *buf, size_t cap, const char **out_path);

/**
 * 就地应用补丁。`expected_qp < 0` 时不校验补丁内 qp 字段。
 */
rkvc_err rkvc_qppatch_apply(uint8_t *base, size_t base_size,
                            const uint8_t *patch, size_t patch_size,
                            int expected_qp);

/** 从文件读入补丁再应用。 */
rkvc_err rkvc_qppatch_apply_file(uint8_t *base, size_t base_size,
                                 const char *patch_path, int expected_qp);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_QPPATCH_H */
