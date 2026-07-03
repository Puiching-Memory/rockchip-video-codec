/**
 * @file buffer.h
 * @brief rkvc v2 统一缓冲区：视频帧（主机 / DMA-BUF）与码流包。
 */

#ifndef RKVC_BUFFER_H
#define RKVC_BUFFER_H

#include "rkvc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 缓冲区种类。 */
typedef enum {
    RKVC_BUF_NONE = 0,          /**< 无效或未初始化 */
    RKVC_BUF_VIDEO,             /**< 视频帧 */
    RKVC_BUF_BITSTREAM,         /**< 压缩码流包 */
} rkvc_buffer_kind;

/** @brief 视频缓冲内存类型。 */
typedef enum {
    RKVC_MEM_HOST = 0,          /**< 主机内存（`rkvc_buffer_alloc_video_host`） */
    RKVC_MEM_DMABUF,            /**< DMA-BUF / DRM PRIME（MPP 硬解输出） */
} rkvc_mem_type;

/** @brief 不透明缓冲区句柄（引用计数）。 */
typedef struct rkvc_buffer rkvc_buffer;

/**
 * @brief 视频帧元数据（只读视图）。
 *
 * `fd` / `modifier` 仅在 `mem_type == RKVC_MEM_DMABUF` 时有效。
 */
typedef struct {
    rkvc_mem_type  mem_type;    /**< 内存类型 */
    int            fd;          /**< DMA-BUF fd，主机内存时为 -1 */
    uint32_t       width;       /**< 帧宽（像素） */
    uint32_t       height;      /**< 帧高（像素） */
    rkvc_pix_fmt   format;      /**< 像素格式 */
    uint32_t       strides[4];  /**< 各平面行跨度 */
    int64_t        pts;         /**< 显示时间戳（任意时间基） */
    uint64_t       modifier;    /**< DRM format modifier（DMA-BUF） */
} rkvc_buffer_video_info;

/**
 * @brief 码流包只读视图。
 *
 * `data` 在 `copy=0` 分配时指向调用方缓冲区，生命周期由调用方保证。
 */
typedef struct {
    const uint8_t *data;        /**< 码流数据指针 */
    size_t         size;        /**< 字节长度 */
    int64_t        pts;         /**< 显示时间戳 */
    int64_t        dts;         /**< 解码时间戳 */
    int            key_frame;   /**< 非 0 表示关键帧 */
} rkvc_buffer_bitstream_view;

/**
 * @brief 增加引用计数。
 * @param buf 缓冲区，可为 NULL。
 * @return 同一指针；`buf` 为 NULL 时返回 NULL。
 */
rkvc_buffer *rkvc_buffer_ref(rkvc_buffer *buf);

/**
 * @brief 减少引用计数；归零时释放底层资源。
 * @param buf 缓冲区，NULL 安全。
 */
void rkvc_buffer_unref(rkvc_buffer *buf);

/**
 * @brief 查询缓冲区种类。
 * @param buf 缓冲区，可为 NULL。
 * @return `RKVC_BUF_NONE` 当 `buf` 为 NULL。
 */
rkvc_buffer_kind rkvc_buffer_kind_of(const rkvc_buffer *buf);

/**
 * @brief 分配主机视频帧（NV12 / YUV420P 等）。
 *
 * 返回的缓冲可直接通过 `rkvc_buffer_get_video_planes` 写入像素。
 *
 * @param out    输出缓冲区指针。
 * @param width  帧宽（像素，> 0）。
 * @param height 帧高（像素，> 0）。
 * @param format 像素格式。
 * @return `RKVC_OK` 或错误码。
 */
rkvc_err rkvc_buffer_alloc_video_host(rkvc_buffer **out,
                                      int width, int height,
                                      rkvc_pix_fmt format);

/**
 * @brief 包装压缩码流为 `rkvc_buffer`。
 *
 * @param out  输出缓冲区指针。
 * @param data 码流数据。
 * @param size 字节长度（> 0）。
 * @param copy 非 0：深拷贝数据；0：零拷贝引用 `data`（调用方须保持有效直至 `unref`）。
 * @return `RKVC_OK` 或错误码。
 */
rkvc_err rkvc_buffer_alloc_bitstream(rkvc_buffer **out,
                                     const uint8_t *data, size_t size,
                                     int copy);

/**
 * @brief 读取视频帧元数据（不映射平面指针）。
 * @param buf  视频缓冲（`RKVC_BUF_VIDEO`）。
 * @param info 输出结构。
 * @return `RKVC_OK`；类型不匹配或参数无效时 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_buffer_get_video_info(const rkvc_buffer *buf,
                                    rkvc_buffer_video_info *info);

/**
 * @brief 获取视频帧平面指针与行跨度（CPU 可写，主机帧）。
 *
 * @param buf     视频缓冲。
 * @param planes  输出：最多 4 个平面 data 指针。
 * @param strides 输出：对应 linesize。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`（无底层 AVFrame）。
 */
rkvc_err rkvc_buffer_get_video_planes(rkvc_buffer *buf,
                                      uint8_t *planes[4],
                                      int strides[4]);

/**
 * @brief 读取码流包视图。
 * @param buf  码流缓冲（`RKVC_BUF_BITSTREAM`）。
 * @param view 输出视图。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_buffer_get_bitstream(const rkvc_buffer *buf,
                                   rkvc_buffer_bitstream_view *view);

/**
 * @brief 设置显示时间戳。
 *
 * 若缓冲关联 AVFrame，同步更新 `av_frame->pts`。
 *
 * @param buf 缓冲区。
 * @param pts 时间戳（调用方自定时间基）。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_buffer_set_pts(rkvc_buffer *buf, int64_t pts);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BUFFER_H */
