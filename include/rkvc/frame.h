/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file frame.h
 * @brief 带引用计数的 host / DMABUF 媒体对象。
 */

#ifndef RKVC_FRAME_H
#define RKVC_FRAME_H

#include <stddef.h>
#include <stdint.h>

#include "rkvc/api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── 帧像素格式（0.4 专用，避免与 0.3 rkvc_pix_fmt 冲突） ────────── */
typedef enum rkvc_frame_fmt {
    RKVC_FRAME_FMT_UNKNOWN = 0,
    RKVC_FRAME_FMT_NV12,      /**< NV12 semi-planar 4:2:0 */
    RKVC_FRAME_FMT_NV21,      /**< NV21 semi-planar 4:2:0（UV 交换） */
    RKVC_FRAME_FMT_YUV420P,   /**< YUV420P planar 4:2:0 */
    RKVC_FRAME_FMT_NV16,      /**< NV16 4:2:2 semi-planar */
    RKVC_FRAME_FMT_P010,      /**< P010 10-bit 4:2:0 */
    RKVC_FRAME_FMT_RGB24,     /**< 8-bit RGB planar */
    RKVC_FRAME_FMT_BITSTREAM, /**< 已封装/裸码流负载 */
} rkvc_frame_fmt;

/* ── 内存域 ───────────────────────────────────────────────────────── */
typedef enum rkvc_mem_domain {
    RKVC_MEM_DOMAIN_HOST = 0,   /**< 可映射的 host 内存 */
    RKVC_MEM_DOMAIN_DMABUF,     /**< dma-buf fd 引用的设备内存 */
} rkvc_mem_domain;

/* ── 帧格式说明（协商对象） ───────────────────────────────────────── */
typedef struct rkvc_frame_spec {
    uint32_t        width;
    uint32_t        height;
    rkvc_frame_fmt  fmt;
    rkvc_mem_domain domain;
    uint32_t        stride;      /**< 主平面行字节数；0 = 库自动计算 */
    uint64_t        modifier;    /**< DRM format modifier；0 = 线性 */
} rkvc_frame_spec;

#define RKVC_FRAME_TS_UNKNOWN INT64_MIN

enum {
    RKVC_FRAME_FLAG_KEYFRAME      = 1u << 0,
    RKVC_FRAME_FLAG_DISCONTINUITY = 1u << 1,
    RKVC_FRAME_FLAG_CORRUPT       = 1u << 2,
};

/** Complete opaque-frame description used for bitstreams and DMA-BUF frames. */
typedef struct rkvc_frame_desc {
    rkvc_header     header;
    rkvc_frame_spec spec;
    void           *data;   /**< HOST pointer; NULL is valid for DMA-BUF. */
    size_t          size;   /**< Payload/allocated bytes; 0 = unknown. */
    int             fd;     /**< DMA-BUF fd, otherwise -1. */
    int64_t         pts;
    int64_t         dts;
    uint32_t        flags;
} rkvc_frame_desc;

void rkvc_frame_desc_init(rkvc_frame_desc *desc, size_t size);

/* ── 帧生命周期 ───────────────────────────────────────────────────── */
/**
 * 引用计数媒体帧。通过 `rkvc_frame_retain`/`rkvc_frame_release` 管理。
 * 帧内容在 release 到 0 前一直有效。
 */
typedef struct rkvc_frame rkvc_frame;

/**
 * @brief 从 host 缓冲包装一个帧（不复制，引用调用方缓冲）。
 * @param spec   目标内存格式（fmt 需为 host 可映射格式）。
 * @param data   宿主缓冲首地址（可为 NULL，用于探测格式）。
 * @param size   缓冲字节数（若无界可传 SIZE_MAX）。
 * @param out    输出帧句柄。
 */
rkvc_status rkvc_frame_wrap_host(const rkvc_frame_spec *spec,
                                 void *data, size_t size,
                                 rkvc_frame **out);

/** Wrap a caller-owned frame description without copying its payload. */
rkvc_status rkvc_frame_wrap(const rkvc_frame_desc *desc, rkvc_frame **out);

/**
 * @brief 递增帧引用计数。
 */
rkvc_frame *rkvc_frame_retain(rkvc_frame *frame);

/**
 * @brief 递减帧引用计数；减到 0 时释放底层资源。
 */
void rkvc_frame_release(rkvc_frame *frame);

/* ── 帧访问（只读，供节点协商/搬运） ─────────────────────────────── */
/** @brief 读取帧的格式与内存描述。 */
rkvc_status rkvc_frame_get_spec(const rkvc_frame *frame,
                                rkvc_frame_spec *spec);

/** @brief 读取帧数据指针（host 域）或 fd（dmabuf 域）。 */
rkvc_status rkvc_frame_get_data(const rkvc_frame *frame,
                                void **data, int *fd);

/** Read payload size, timing, flags and memory handles in one call. */
rkvc_status rkvc_frame_get_desc(const rkvc_frame *frame,
                                rkvc_frame_desc *desc);

/** @brief 读取帧的引用计数（调试用）。 */
uint32_t rkvc_frame_ref_count(const rkvc_frame *frame);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_FRAME_H */
