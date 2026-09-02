/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file qppatch.h
 * @brief MLVC QPP1 差量补丁（内存版，布局与 tools/mlvc/qppatch.py 一致）。
 *
 * QPP1 把基准 RKNN 模型按 qp 精调后的字节差量编码为 range 列表，
 * 应用时 memcpy 覆盖基准模型内偏移。基准 CRC32 校验防止错配。
 */

#ifndef RKVC_BACKEND_MLVC_QPPATCH_H
#define RKVC_BACKEND_MLVC_QPPATCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MLVC_QPPATCH_MAGIC        "QPP1"
#define MLVC_QPPATCH_VERSION      1u
#define MLVC_QPPATCH_HEADER_SIZE  48u
#define MLVC_QPPATCH_RANGE_SIZE   8u

/**
 * @brief 把 QPP1 补丁应用到基准模型缓冲（就地修改）。
 *
 * @param base        基准模型字节（可写；补丁范围 memcpy 覆盖）。
 * @param base_size   基准字节数。
 * @param patch       补丁载荷字节（来自 .rkmodel QPPATCH 载荷）。
 * @param patch_size  补丁字节数。
 * @param expected_qp 期望 qp；<0 跳过校验。
 * @return 0 成功；负 rkvc_status。
 */
int mlvc_qppatch_apply(uint8_t *base, size_t base_size,
                       const uint8_t *patch, size_t patch_size,
                       int expected_qp);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_QPPATCH_H */
