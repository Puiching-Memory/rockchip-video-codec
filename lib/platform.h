/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file platform.h
 * @brief 运行时平台探测（lib/ 内部使用）。
 *
 * 无任何预设板卡知识：SoC 名、NPU、RGA、VPU 编解码能力全部来自
 * 运行时系统信息（device-tree / rknpu debugfs / /dev/rga / MPP 内核
 * 能力位）。
 */

#ifndef RKVC_INTERNAL_PLATFORM_H
#define RKVC_INTERNAL_PLATFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 运行时探测到的平台硬件事实。 */
typedef struct {
    char soc[32];        /**< SoC 名（如 "rk3588"，取自 device-tree）；未知为空串 */
    int has_npu;         /**< NPU 存在（rknpu debugfs 节点或 DRI NPU 节点） */
    int npu_cores;       /**< NPU 核心数（rknpu 驱动按核负载计数）；未知为 0 */
    int has_rga;         /**< RGA 存在（/dev/rga） */
    int vpu_h264_enc_hw; /**< VPU 硬件 H.264 编码（MPP 内核能力位探测） */
    int vpu_hevc_enc_hw; /**< VPU 硬件 HEVC 编码 */
    int vpu_h264_dec_hw; /**< VPU 硬件 H.264 解码 */
    int vpu_hevc_dec_hw; /**< VPU 硬件 HEVC 解码 */
    int vpu_av1_dec_hw;  /**< VPU 硬件 AV1 解码 */
} rkvc_platform_info;

/**
 * @brief 探测当前平台（首次调用时执行一次并缓存，线程安全）。
 * @return 静态只读结果，进程生命周期内有效。
 */
const rkvc_platform_info *rkvc_platform_probe(void);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_INTERNAL_PLATFORM_H */
