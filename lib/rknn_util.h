/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rknn_util.h
 * @brief RKNN 运行时共享辅助（调用方须先包含 <rknn_api.h>）。
 */

#ifndef RKVC_INTERNAL_RKNN_UTIL_H
#define RKVC_INTERNAL_RKNN_UTIL_H

/**
 * NPU 核心数 → RKNN core_mask 映射。
 * 平台探测只给出硬件事实（npu_cores），RKNN 常量在此（RKNN 专用层）
 * 映射，避免平台探测层（platform.h，不依赖 rknn_api.h）耦合 RKNN 枚举。
 */
static inline rknn_core_mask rkvc_rknn_npu_core_mask(int npu_cores)
{
    switch (npu_cores) {
    case 3:  return RKNN_NPU_CORE_0_1_2;
    case 2:  return RKNN_NPU_CORE_0_1;
    default: return RKNN_NPU_CORE_0;
    }
}

/**
 * 自适应启用多核 NPU：从 hint 核心数向下尝试 core_mask，
 * 以 RKNN 驱动为最终权威（如 RK3576 双核拒绝 0x7，自动降到 0x3）。
 * hint 取自板卡 profile 的运行时探测值；全部失败时返回 0，
 * 保持运行时默认单核/AUTO 行为（单核平台原本即不显式设置）。
 *
 * @param ctx        已初始化的 rknn_context。
 * @param hint_cores 期望启用的核心数（>3 按 3 处理）。
 * @return 实际生效的核心数（>1）；0 表示保持运行时默认。
 */
static inline int rkvc_rknn_apply_npu_cores(rknn_context ctx, int hint_cores)
{
    if (hint_cores > 3)
        hint_cores = 3;
    for (int c = hint_cores; c > 1; c--) {
        if (rknn_set_core_mask(ctx, rkvc_rknn_npu_core_mask(c)) == RKNN_SUCC)
            return c;
    }
    return 0;
}

#endif /* RKVC_INTERNAL_RKNN_UTIL_H */
