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

/* ── 帧像素格式 ───────────────────────────────────────────────── */
/** @brief 帧像素格式（原始平面格式与码流负载）。 */
typedef enum rkvc_frame_fmt {
    RKVC_FRAME_FMT_UNKNOWN = 0, /**< 未知；端口协商时的通配值 */
    RKVC_FRAME_FMT_NV12,      /**< NV12 semi-planar 4:2:0 */
    RKVC_FRAME_FMT_NV21,      /**< NV21 semi-planar 4:2:0（UV 交换） */
    RKVC_FRAME_FMT_YUV420P,   /**< YUV420P planar 4:2:0 */
    RKVC_FRAME_FMT_NV16,      /**< NV16 4:2:2 semi-planar */
    RKVC_FRAME_FMT_P010,      /**< P010 10-bit 4:2:0 */
    RKVC_FRAME_FMT_RGB24,     /**< 8-bit RGB planar */
    RKVC_FRAME_FMT_BITSTREAM, /**< 已封装/裸码流负载 */
} rkvc_frame_fmt;

/* ── 内存域 ───────────────────────────────────────────────────────── */
/** @brief 帧数据所在的内存域。 */
typedef enum rkvc_mem_domain {
    RKVC_MEM_DOMAIN_HOST = 0,   /**< 可映射的 host 内存 */
    RKVC_MEM_DOMAIN_DMABUF,     /**< dma-buf fd 引用的设备内存 */
} rkvc_mem_domain;

/* ── 帧格式说明（协商对象） ───────────────────────────────────────── */
/**
 * @brief 帧格式说明：图连接两侧的协商对象。
 *
 * 宽高/stride 为 0 表示通配，协商时由对端或库填充。
 */
typedef struct rkvc_frame_spec {
    uint32_t        width;       /**< 可见宽度（像素） */
    uint32_t        height;      /**< 可见高度（像素） */
    rkvc_frame_fmt  fmt;         /**< 像素格式 */
    rkvc_mem_domain domain;      /**< 内存域 */
    uint32_t        stride;      /**< 主平面行字节数；0 = 库自动计算 */
    uint32_t        ver_stride;  /**< 垂直步幅（UV 平面偏移 = stride*ver_stride）；0 = height */
    uint64_t        modifier;    /**< DRM format modifier；0 = 线性 */
} rkvc_frame_spec;

/** @brief 未知时间戳的哨兵值（无时间戳或尚未输出）。 */
#define RKVC_FRAME_TS_UNKNOWN INT64_MIN

/** @brief 帧标志位（按位或组合）。 */
enum {
    RKVC_FRAME_FLAG_KEYFRAME      = 1u << 0, /**< 关键帧/IDR */
    RKVC_FRAME_FLAG_DISCONTINUITY = 1u << 1, /**< 时间戳或流不连续 */
    RKVC_FRAME_FLAG_CORRUPT       = 1u << 2, /**< 解码错误/被丢弃 */
};

/** MPP H.264/HEVC 传统 ROI 路径最多支持 8 个矩形。 */
#define RKVC_ROI_MAX_REGIONS 8u

/**
 * @brief 单帧编码兴趣区域（ROI）。
 *
 * 坐标使用可见帧像素。`qp_delta` 相对帧 QP：负值提升该区域质量，
 * 正值减少该区域码率。MPP 后端在提交前把矩形对齐到 16 像素。
 */
typedef struct rkvc_roi_region {
    uint32_t x;              /**< 区域左上角 X（可见像素） */
    uint32_t y;              /**< 区域左上角 Y（可见像素） */
    uint32_t width;          /**< 区域宽度（像素） */
    uint32_t height;         /**< 区域高度（像素） */
    int16_t  qp_delta;    /**< 相对 QP，取值 [-51, 51] */
    uint8_t  force_intra; /**< 非零 = 区域内强制帧内编码 */
    uint8_t  reserved;    /**< 恒为 0 */
} rkvc_roi_region;

/**
 * @brief 编码本帧前的可选运行时控制。
 *
 * 0 表示“保持当前值”。变更后的码率/GOP 对后续帧持续生效；
 * `force_idr` 只影响本帧。
 */
typedef struct rkvc_encode_control {
    int32_t  bitrate_bps; /**< 新目标码率；0 = 保持当前值 */
    uint32_t gop_size;    /**< 新 GOP；0 = 保持当前值 */
    uint8_t  force_idr;   /**< 非零 = 本帧编为 IDR */
    uint8_t  reserved[7]; /**< 恒为 0 */
} rkvc_encode_control;

/** @brief 完整帧描述：码流帧与 DMA-BUF 帧的包装载体。 */
typedef struct rkvc_frame_desc {
    rkvc_header     header;      /**< struct_size/api_version */
    rkvc_frame_spec spec;        /**< 格式与内存域描述 */
    void           *data;   /**< HOST 域数据指针；DMA-BUF 帧可为 NULL */
    size_t          size;   /**< 载荷/分配字节数；0 = 未知 */
    int             fd;     /**< DMA-BUF fd；非 DMABUF 域为 -1 */
    int64_t         pts;    /**< 显示时间戳；RKVC_FRAME_TS_UNKNOWN = 未知 */
    int64_t         dts;    /**< 解码时间戳；RKVC_FRAME_TS_UNKNOWN = 未知 */
    uint32_t        flags;  /**< RKVC_FRAME_FLAG_* 组合 */
    const rkvc_roi_region *roi_regions; /**< rkvc_frame_wrap() 会深拷贝 */
    size_t          roi_region_count;   /**< 0..RKVC_ROI_MAX_REGIONS */
    rkvc_encode_control encode;         /**< 逐帧可选编码控制 */
} rkvc_frame_desc;

/** @brief 初始化帧描述（填充头部、fd=-1、时间戳置未知）。 */
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

/** @brief 包装调用方持有的帧描述（不拷贝载荷，借用其内存句柄）。 */
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

/** @brief 一次读出载荷大小、时间戳、标志与内存句柄。 */
rkvc_status rkvc_frame_get_desc(const rkvc_frame *frame,
                                rkvc_frame_desc *desc);

/** @brief 读取帧的引用计数（调试用）。 */
uint32_t rkvc_frame_ref_count(const rkvc_frame *frame);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_FRAME_H */
