/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file board.h
 * @brief 板卡/SoC 平台抽象。
 *
 * rkvc 原生仅针对 RK3588，现正扩展为多板卡架构。各板卡的硬件差异
 * （最大编/解分辨率、VPU 编解码能力、NPU、RGA 等）统一由 *板卡 profile*
 * 描述，避免在源码中散落硬编码（如旧的 `7680×4320`）。
 *
 * 板卡选取优先级：
 *   1. 运行时环境变量 `RKVC_BOARD`（强制指定，用于测试/CI）
 *   2. `/proc/device-tree/compatible` 自动探测
 *   3. 编译期默认 `RKVC_DEFAULT_BOARD`（CMake `RKVC_BOARD`，默认 `rk3588`）
 */

#ifndef RKVC_BOARD_H
#define RKVC_BOARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 已知板卡/SoC 标识。 */
typedef enum {
    RKVC_BOARD_UNKNOWN = 0, /**< 未知或探测失败 */
    RKVC_BOARD_RK3588,      /**< Rockchip RK3588（旗舰 8K SoC，8 核） */
    RKVC_BOARD_RV1126B,     /**< Rockchip RV1126B（4K AI 视觉 SoC，4×Cortex-A53 + 3TOPS NPU） */
} rkvc_board_id;

/**
 * @brief 将板卡标识转为稳定名称（如 `"rk3588"`、`"rv1126b"`）。
 * @param id 板卡标识。
 * @return 静态字符串；`RKVC_BOARD_UNKNOWN` 返回 `"unknown"`。
 */
const char *rkvc_board_id_name(rkvc_board_id id);

/**
 * @brief 将名称解析为板卡标识（不区分大小写）。
 * @param name 板卡名称（如 `"rk3588"`、`"rv1126b"`）。
 * @return 板卡标识；未知名称返回 `RKVC_BOARD_UNKNOWN`。
 */
rkvc_board_id rkvc_board_id_from_name(const char *name);

/**
 * @brief 运行时探测当前板卡。
 *
 * 按上述优先级解析；探测不到时返回编译期默认板卡。
 * 线程安全（首次探测后缓存结果）。
 *
 * @return 当前板卡标识（至少为编译期默认值）。
 */
rkvc_board_id rkvc_detect_board(void);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BOARD_H */
