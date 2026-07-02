/**
 * @file rkvc.h
 * @brief RK3588 Video Codec Library v2 — Session / Pipeline API
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

#ifdef __cplusplus
extern "C" {
#endif

const char *rkvc_version(void);
uint32_t rkvc_version_number(void);

rkvc_err rkvc_init(void);
void rkvc_deinit(void);
const char *rkvc_err_str(rkvc_err err);

/** 日志级别与 FFmpeg AV_LOG_* 常量一致（如 AV_LOG_INFO、AV_LOG_DEBUG）。 */
void rkvc_set_log_level(int level);
int rkvc_get_log_level(void);

/**
 * 计算文件摘要（algo 支持 md5、sha256 等，见 av_hash_names）。
 * out_hex 长度须 >= 2 * digest_size + 1。
 */
rkvc_err rkvc_hash_file(const char *path, const char *algo,
                        char *out_hex, size_t out_size);

typedef enum {
    RKVC_INPUT_UNKNOWN = 0,
    RKVC_INPUT_RAW_VIDEO,
    RKVC_INPUT_COMPRESSED_VIDEO,
} rkvc_input_format_probe;

rkvc_input_format_probe rkvc_probe_input_format(const uint8_t *data,
                                                size_t size);

typedef struct {
    int has_h264_enc;
    int has_hevc_enc;
    int has_av1_enc;      /**< SVT-AV1 */
    int has_h264_dec;
    int has_hevc_dec;
    int has_av1_dec;
    int has_dma_heap;
    int has_rga;
    int max_width;
    int max_height;
} rkvc_caps;

rkvc_err rkvc_query_caps(rkvc_caps *caps);
rkvc_err rkvc_check_hw_permissions(void);

int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out);
const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo);

/** YUV420p 平面缓冲上采样（RGA 硬件）。 */
rkvc_err rkvc_upscale_yuv420p(const uint8_t *src, uint8_t *dst,
                              int src_w, int src_h,
                              int dst_w, int dst_h,
                              rkvc_upscale_algo algo);

/** NV12 紧凑平面缓冲上采样（RGA 硬件，无 YUV420p 转换）。 */
rkvc_err rkvc_upscale_nv12(const uint8_t *src, uint8_t *dst,
                           int src_w, int src_h,
                           int dst_w, int dst_h,
                           rkvc_upscale_algo algo);

/**
 * 复用 RGA import 的批量上采样上下文（固定 src/dst 缓冲，避免每帧 import/release）。
 * 适用于 bench 文件批处理与 Session 管线内多帧缩放。
 */
typedef struct rkvc_upscale_ctx rkvc_upscale_ctx;

rkvc_upscale_ctx *rkvc_upscale_ctx_create(int src_w, int src_h,
                                          int dst_w, int dst_h,
                                          rkvc_upscale_algo algo);
void rkvc_upscale_ctx_destroy(rkvc_upscale_ctx *ctx);

/** 内部 NV12 源/目的缓冲（紧凑布局，可直接 pread / pwrite）。 */
uint8_t *rkvc_upscale_ctx_src_buf(rkvc_upscale_ctx *ctx);
uint8_t *rkvc_upscale_ctx_dst_buf(rkvc_upscale_ctx *ctx);
size_t rkvc_upscale_ctx_src_bytes(const rkvc_upscale_ctx *ctx);
size_t rkvc_upscale_ctx_dst_bytes(const rkvc_upscale_ctx *ctx);

/** 对 ctx 内部缓冲执行一次 RGA 缩放。 */
rkvc_err rkvc_upscale_ctx_process(rkvc_upscale_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_H */
