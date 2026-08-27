/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc.h
 * @brief Rockchip 多 SoC 视频编解码库 — 主入口（包含全部公共头文件）。
 */

#ifndef RKVC_H
#define RKVC_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/types.h"
#include "rkvc/buffer.h"
#include "rkvc/pipeline.h"
#include "rkvc/policy.h"
#include "rkvc/port.h"
#include "rkvc/session.h"
#include "rkvc/roi.h"
#include "rkvc/runtime.h"
#include "rkvc/net.h"
#include "rkvc/reconfig.h"
#include "rkvc/license.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 库版本字符串（与 CMake `project(VERSION)` 一致）。
 */
const char *rkvc_version(void);

/**
 * @brief 库版本号：`major<<16 | minor<<8 | patch`。
 */
uint32_t rkvc_version_number(void);

/**
 * @brief 全局初始化（线程安全，可多次调用；内部 `pthread_once`）。
 *
 * 注册 FFmpeg 日志回调、探测硬件环境。Session 创建时也会隐式调用。
 *
 * @return 恒为 `RKVC_OK`（初始化失败时进程级 abort 极少见）。
 */
rkvc_err rkvc_init(void);

/**
 * @brief 标记库为未初始化状态。
 *
 * 不调用 `avformat_network_deinit()` 等 FFmpeg 全局反初始化，
 * 避免多实例共享状态时崩溃。进程退出前可选调用。
 */
void rkvc_deinit(void);

/**
 * @brief 将错误码转为静态描述字符串。
 * @param err `rkvc_err` 值。
 * @return 人类可读英文描述；未知码返回 `"unknown"`。
 */
const char *rkvc_err_str(rkvc_err err);

/**
 * @brief 设置日志级别。
 *
 * 级别与 FFmpeg `AV_LOG_*` 常量一致（如 `AV_LOG_INFO`、`AV_LOG_DEBUG`）。
 * 同时作用于 rkvc 自身日志与 FFmpeg `av_log`。
 *
 * @param level FFmpeg 日志级别常量。
 */
void rkvc_set_log_level(int level);

/**
 * @brief 获取当前日志级别。
 * @return FFmpeg `AV_LOG_*` 级别。
 */
int rkvc_get_log_level(void);

/** @brief 输入数据格式探测结果。 */
typedef enum {
    RKVC_INPUT_UNKNOWN = 0,         /**< 无法判定或非压缩头 */
    RKVC_INPUT_RAW_VIDEO,           /**< 保留：当前探测逻辑不返回此值 */
    RKVC_INPUT_COMPRESSED_VIDEO,    /**< 检测到容器/码流魔数（MP4/MKV/Annex-B 等） */
} rkvc_input_format_probe;

/**
 * @brief 根据文件头前几字节探测输入格式。
 *
 * 用于防止将 MP4 等误作原始 NV12 打开。未识别压缩格式时返回 `RKVC_INPUT_UNKNOWN`
 * （原始 YUV 无魔数，需由调用方按扩展名/参数判定）。
 *
 * @param data 文件头数据。
 * @param size 可用字节数（建议 ≥ 12）。
 * @return 探测结果枚举。
 */
rkvc_input_format_probe rkvc_probe_input_format(const uint8_t *data,
                                                size_t size);

/**
 * @brief 运行时硬件与编解码器能力。
 *
 * 编解码 `has_*` 字段在设备权限不足时为 0；`rkvc_info -j` JSON 字段与此对应。
 */
typedef struct {
    char soc[32];       /**< 探测到的 SoC 名（如 "rk3588"，取自 device-tree）；未知为空串 */
    int has_h264_enc;   /**< `h264_rkmpp` 编码可用 */
    int has_hevc_enc;   /**< `hevc_rkmpp` 编码可用 */
    int has_av1_enc;    /**< SVT-AV1 编码可用（软件，与平台无关） */
    int has_h264_dec;   /**< `h264_rkmpp` 解码可用 */
    int has_hevc_dec;   /**< `hevc_rkmpp` 解码可用 */
    int has_av1_dec;    /**< `av1_rkmpp` 解码可用 */
    int has_dma_heap;   /**< `/dev/dma_heap/` 下存在可访问节点 */
    int has_rga;        /**< `/dev/rga` 可访问 */
    int has_rknn;       /**< RKNN 已编译、NPU/驱动节点可访问，且 RGA 可用（`rkvc_sr`） */
} rkvc_caps;

/**
 * @brief 查询能力与编解码器注册情况。
 * @param caps 输出结构（不可为 NULL）。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_query_caps(rkvc_caps *caps);

/**
 * @brief 检查 MPP / DMA-BUF / RGA 设备节点访问权限。
 *
 * @return `RKVC_OK` 或 `RKVC_ERR_PERMISSION`。
 */
rkvc_err rkvc_check_hw_permissions(void);

/**
 * @brief 将 CLI 风格名称解析为上采样算法。
 *
 * 支持：`none`、`nearest`、`bilinear`、`bicubic`、`rkvc_sr`（映射 `RKVC_UPSCALE_AI_SR`）。
 *
 * @param name 算法名（不区分大小写由调用方处理；库内区分大小写）。
 * @param out  输出枚举。
 * @return 0 成功，-1 未知名称或参数无效。
 */
int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out);

/**
 * @brief 将上采样算法转为 CLI 风格名称。
 * @return 静态字符串；未知枚举返回 `"unknown"`。
 */
const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo);

/**
 * @brief YUV420P 平面缓冲一次性 RGA 缩放（无格式转换开销外的额外拷贝）。
 *
 * 不支持 `RKVC_UPSCALE_NONE` / `RKVC_UPSCALE_AI_SR`。
 *
 * @return `RKVC_OK`、`RKVC_ERR_INVALID`、`RKVC_ERR_HW`（RGA 不可用）。
 */
rkvc_err rkvc_upscale_yuv420p(const uint8_t *src, uint8_t *dst,
                              int src_w, int src_h,
                              int dst_w, int dst_h,
                              rkvc_upscale_algo algo);

/**
 * @brief NV12 紧凑平面缓冲一次性 RGA 缩放。
 *
 * 缓冲区布局：`width*height` 字节 Y + `width*height/2` 字节 UV。
 * 不支持 `RKVC_UPSCALE_NONE` / `RKVC_UPSCALE_AI_SR`。
 */
rkvc_err rkvc_upscale_nv12(const uint8_t *src, uint8_t *dst,
                           int src_w, int src_h,
                           int dst_w, int dst_h,
                           rkvc_upscale_algo algo);

/**
 * @brief 复用 RGA import 的批量上采样上下文（固定 src/dst 缓冲）。
 *
 * 避免每帧 `importbuffer` / `releasebuffer`，适用于 bench 批处理与 Session 内多帧缩放。
 * 内部缓冲为 NV12 紧凑布局，可通过 `rkvc_upscale_ctx_src_buf` / `_dst_buf` 直接读写。
 */
typedef struct rkvc_upscale_ctx rkvc_upscale_ctx;

/**
 * @brief 创建上采样上下文。
 * @return 上下文指针，失败返回 NULL（RGA 不可用或参数无效）。
 */
rkvc_upscale_ctx *rkvc_upscale_ctx_create(int src_w, int src_h,
                                          int dst_w, int dst_h,
                                          rkvc_upscale_algo algo);

/** @brief 释放上下文及内部 RGA/DMA 资源。NULL 安全。 */
void rkvc_upscale_ctx_destroy(rkvc_upscale_ctx *ctx);

/** @brief 内部源 NV12 缓冲（可直接 pread / memcpy 写入）。 */
uint8_t *rkvc_upscale_ctx_src_buf(rkvc_upscale_ctx *ctx);

/** @brief 内部目的 NV12 缓冲（可直接 pwrite / memcpy 读出）。 */
uint8_t *rkvc_upscale_ctx_dst_buf(rkvc_upscale_ctx *ctx);

/** @brief 源帧字节数（NV12：`src_w * src_h * 3 / 2`）。 */
size_t rkvc_upscale_ctx_src_bytes(const rkvc_upscale_ctx *ctx);

/** @brief 目的帧字节数。 */
size_t rkvc_upscale_ctx_dst_bytes(const rkvc_upscale_ctx *ctx);

/**
 * @brief 对上下文内部缓冲执行一次 RGA 缩放（src → dst）。
 * @return `RKVC_OK` 或 `RKVC_ERR_HW` / `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_upscale_ctx_process(rkvc_upscale_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_H */
