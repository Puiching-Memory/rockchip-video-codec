/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file internal.h
 * @brief rkvc v2 内部共享头文件。
 */

#ifndef RKVC_INTERNAL_H
#define RKVC_INTERNAL_H

#include "rkvc/rkvc.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/fifo.h>
#include <libavutil/hash.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

#define AV_PIX_FMT_RKMPP AV_PIX_FMT_DRM_PRIME

/** SVT-AV1 近实时/高性能 preset（与 bench SVT_PRESET / QUALITY 默认 11 对齐） */
#define RKVC_SVT_PRESET_PERF 11
/** SVT-AV1 非实时高质量 preset（与 bench SVT_HQ_PRESET / OFFLINE 默认 4 对齐，≥1fps@1080p） */
#define RKVC_SVT_PRESET_HQ 4
/** SVT-AV1 lp：0 表示由编码器按 CPU 核数自动选择 */
#define RKVC_SVT_LP_AUTO 0

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void rkvc_log_print(int level, const char *fmt, ...) av_printf_format(2, 3);
void rkvc_ffmpeg_utils_init(void);
void rkvc_set_log_level(int level);
int rkvc_get_log_level(void);
int64_t rkvc_now_us(void);

#define RKVC_LOG(fmt, ...) \
    rkvc_log_print(AV_LOG_INFO, fmt "\n", ##__VA_ARGS__)

/* ── 工具 ─────────────────────────────────────────────────────────── */

rkvc_err rkvc_from_averror(int av_err);
void *rkvc_malloc(size_t size);
void *rkvc_calloc(size_t nmemb, size_t size);
void rkvc_free(void *ptr);

#ifdef RKVC_ENABLE_FAULT_INJECTION
void rkvc_test_fail_alloc_after(long countdown);
void rkvc_test_clear_faults(void);
long rkvc_test_alloc_count(void);
#endif

rkvc_err rkvc_get_hw_device_ctx(AVBufferRef **out);
int rkvc_is_valid_pix_fmt(rkvc_pix_fmt fmt);
int rkvc_is_valid_rc_mode(rkvc_rc_mode mode);
enum AVPixelFormat rkvc_to_av_pix_fmt(rkvc_pix_fmt fmt);
rkvc_pix_fmt rkvc_from_av_pix_fmt(enum AVPixelFormat fmt);
int rkvc_buffer_looks_compressed_video(const uint8_t *data, size_t size);
rkvc_err rkvc_avframe_alloc_contiguous(AVFrame *av_frame);

rkvc_err rkvc_dict_parse_opts(AVDictionary **dict, const char *opts);
void rkvc_dict_free(AVDictionary **dict);
rkvc_err rkvc_dict_set_int(AVDictionary **dict, const char *key, int64_t val);
rkvc_err rkvc_opt_set_dict(void *obj, AVDictionary **dict);
rkvc_err rkvc_codec_open2(AVCodecContext *ctx, const AVCodec *codec,
                          AVDictionary **opts, const char *ctx_name);
rkvc_err rkvc_format_open_input(AVFormatContext **fmt, const char *path,
                                AVDictionary **opts);
rkvc_err rkvc_hash_buffer(const char *algo, const uint8_t *data, size_t len,
                          char *out_hex, size_t out_size);
rkvc_err rkvc_hash_file(const char *path, const char *algo,
                        char *out_hex, size_t out_size);

/* ── Buffer ───────────────────────────────────────────────────────── */

struct rkvc_buffer {
    rkvc_buffer_kind  kind;
    int               ref_count;
    pthread_mutex_t   lock;

    rkvc_mem_type     mem_type;
    int               fd;
    void             *mmap_base;   /**< dma-heap mmap 基址（仅自分配 DMABUF，buffer_free 负责 munmap） */
    size_t            mmap_size;   /**< mmap 映射字节数 */
    uint32_t          width;
    uint32_t          height;
    rkvc_pix_fmt      format;
    uint32_t          strides[4];
    uint64_t          modifier;
    int64_t           pts;

    AVFrame          *av_frame;
    int               owns_avframe;

    uint8_t          *data;
    size_t            size;
    int64_t           dts;
    int               key_frame;
    int               owns_data;
};

rkvc_buffer *rkvc_buffer_wrap_avframe(AVFrame *frame, int take_ownership);
rkvc_buffer *rkvc_buffer_from_drm_frame(AVFrame *hw_frame);
rkvc_buffer *rkvc_buffer_from_avpacket(const AVPacket *avpkt);
rkvc_err rkvc_buffer_to_host_frame(rkvc_buffer *buf, AVFrame **frame_out);

/* ── Buffer pool (DMA heap) ───────────────────────────────────────── */

typedef struct rkvc_buffer_pool rkvc_buffer_pool;

rkvc_buffer_pool *rkvc_buffer_pool_create(void);
void rkvc_buffer_pool_destroy(rkvc_buffer_pool *pool);
rkvc_err rkvc_buffer_pool_alloc_video(rkvc_buffer_pool *pool,
                                      rkvc_buffer **out,
                                      int width, int height,
                                      rkvc_pix_fmt format,
                                      rkvc_mem_type mem_type);

/* ── Port queue ───────────────────────────────────────────────────── */

#define RKVC_PORT_QUEUE_DEFAULT 3

typedef struct rkvc_port_queue rkvc_port_queue;

struct rkvc_port {
    char              name[32];
    rkvc_port_queue  *queue;
    rkvc_session     *session;
};

rkvc_port_queue *rkvc_port_queue_create(int capacity);
void rkvc_port_queue_destroy(rkvc_port_queue *q);
rkvc_err rkvc_port_queue_push(rkvc_port_queue *q, rkvc_buffer *buf);
rkvc_err rkvc_port_queue_pull(rkvc_port_queue *q, rkvc_buffer **buf,
                              int timeout_ms);

/* ── Nodes ────────────────────────────────────────────────────────── */

typedef struct rkvc_demux rkvc_demux;
typedef struct rkvc_mux rkvc_mux;
typedef struct rkvc_mpp_dec rkvc_mpp_dec;
typedef struct rkvc_mpp_enc rkvc_mpp_enc;
typedef struct rkvc_svt_enc rkvc_svt_enc;
typedef struct rkvc_v4l2_cap rkvc_v4l2_cap;

typedef struct {
    const char *input_path;
    const char *format_opts; /**< avformat_open_input 选项，key=val:key2=val2 */
} rkvc_demux_config;

typedef struct {
    const char       *output_path;
    const rkvc_route_plan *route;
    int               width;
    int               height;
    int               fps_num;
    int               fps_den;
    int64_t           bitrate;
    rkvc_pix_fmt      pixel_format;
    int               gop_size;
} rkvc_mux_config;

typedef struct {
    const rkvc_route_plan *route;
    rkvc_pix_fmt      output_format;
    int               low_latency;
    const char       *codec_opts; /**< avcodec_open2 选项，key=val:key2=val2 */
} rkvc_mpp_dec_config;

typedef struct {
    const rkvc_route_plan *route;
    int               width;
    int               height;
    int               fps_num;
    int               fps_den;
    int64_t           bitrate;
    rkvc_pix_fmt      input_format;
    int               gop_size;
    int               low_latency;
    rkvc_rc_mode      rc_mode;
    int               qp_init;
    const char       *codec_opts; /**< 编码器 priv_data / open2 选项 */
} rkvc_mpp_enc_config;

typedef struct {
    int               width;
    int               height;
    int               fps_num;
    int               fps_den;
    int64_t           bitrate;
    rkvc_pix_fmt      input_format;
    int               gop_size;
    int               svt_preset;
    int               svt_lp;
    int               svt_rtc;
    rkvc_rc_mode      rc_mode;
} rkvc_svt_enc_config;

typedef struct {
    const char *device;
    int         width;
    int         height;
    int         fps_num;
    int         fps_den;
} rkvc_v4l2_config;

rkvc_err rkvc_demux_open(rkvc_demux **out, const rkvc_demux_config *cfg);
void rkvc_demux_close(rkvc_demux *d);
rkvc_err rkvc_demux_read_packet(rkvc_demux *d, rkvc_buffer **pkt);
int rkvc_demux_video_stream_index(const rkvc_demux *d);
AVCodecParameters *rkvc_demux_video_par(rkvc_demux *d);

rkvc_err rkvc_mux_open(rkvc_mux **out, const rkvc_mux_config *cfg,
                       AVCodecParameters *src_par);
void rkvc_mux_close(rkvc_mux *m);
rkvc_err rkvc_mux_write_packet(rkvc_mux *m, const rkvc_buffer *pkt);

rkvc_err rkvc_mpp_dec_open(rkvc_mpp_dec **out, const rkvc_mpp_dec_config *cfg,
                           AVCodecParameters *par);
void rkvc_mpp_dec_close(rkvc_mpp_dec *dec);
rkvc_err rkvc_mpp_dec_send_packet(rkvc_mpp_dec *dec, const rkvc_buffer *pkt);
rkvc_err rkvc_mpp_dec_receive_frame(rkvc_mpp_dec *dec, rkvc_buffer **frame);
rkvc_err rkvc_mpp_dec_drain(rkvc_mpp_dec *dec);

rkvc_err rkvc_mpp_enc_open(rkvc_mpp_enc **out, const rkvc_mpp_enc_config *cfg);
void rkvc_mpp_enc_close(rkvc_mpp_enc *enc);
rkvc_err rkvc_mpp_enc_send_frame(rkvc_mpp_enc *enc, rkvc_buffer *frame);
/**
 * 发送帧并附带 ROI（写入 AVFrame REGIONS_OF_INTEREST，由 rkmppenc 桥到 MPP）。
 * `rois==NULL` 或 `roi_count==0` 时行为同 `rkvc_mpp_enc_send_frame`。
 */
rkvc_err rkvc_mpp_enc_send_frame_roi(rkvc_mpp_enc *enc, rkvc_buffer *frame,
                                     const rkvc_roi_rect *rois, int roi_count);
/** 更新 AVCodecContext bit_rate/gop；下一帧由 rkmppenc 推到 MPP。 */
rkvc_err rkvc_mpp_enc_apply_rc(rkvc_mpp_enc *enc, int64_t bitrate, int gop_size);
/** 发送帧；`force_idr!=0` 时将该帧标为 I 以触发 MPP IDR。 */
rkvc_err rkvc_mpp_enc_send_frame_roi_ex(rkvc_mpp_enc *enc, rkvc_buffer *frame,
                                        const rkvc_roi_rect *rois, int roi_count,
                                        int force_idr);
rkvc_err rkvc_mpp_enc_receive_packet(rkvc_mpp_enc *enc, rkvc_buffer **pkt);
rkvc_err rkvc_mpp_enc_drain(rkvc_mpp_enc *enc);

rkvc_err rkvc_svt_enc_open(rkvc_svt_enc **out, const rkvc_svt_enc_config *cfg);
void rkvc_svt_enc_close(rkvc_svt_enc *enc);
rkvc_err rkvc_svt_enc_send_frame(rkvc_svt_enc *enc, rkvc_buffer *frame);
rkvc_err rkvc_svt_enc_receive_packet(rkvc_svt_enc *enc, rkvc_buffer **pkt);
rkvc_err rkvc_svt_enc_drain(rkvc_svt_enc *enc);
rkvc_err rkvc_svt_enc_write_header(rkvc_svt_enc *enc, AVCodecParameters *par);

rkvc_err rkvc_v4l2_open(rkvc_v4l2_cap **out, const rkvc_v4l2_config *cfg);
void rkvc_v4l2_close(rkvc_v4l2_cap *c);
rkvc_err rkvc_v4l2_read_frame(rkvc_v4l2_cap *c, rkvc_buffer **out,
                              int timeout_ms);
void rkvc_v4l2_get_size(const rkvc_v4l2_cap *c, int *w, int *h);

rkvc_err rkvc_rga_scale_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                               int dst_w, int dst_h, rkvc_pix_fmt dst_fmt,
                               rkvc_upscale_algo algo);
rkvc_err rkvc_post_upscale_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                                  int dst_w, int dst_h,
                                  rkvc_upscale_algo algo,
                                  const char *rkvc_sr_model_path);
const char *rkvc_upscale_algo_name(rkvc_upscale_algo algo);
int rkvc_upscale_algo_from_name(const char *name, rkvc_upscale_algo *out);
rkvc_err rkvc_dma_to_host(const rkvc_buffer *src, rkvc_buffer **dst);
rkvc_err rkvc_buffer_dmabuf_begin_cpu_read(const rkvc_buffer *buf);
rkvc_err rkvc_buffer_dmabuf_end_cpu_read(const rkvc_buffer *buf);
rkvc_err rkvc_buffer_dmabuf_begin_device_write(const rkvc_buffer *buf);
rkvc_err rkvc_buffer_dmabuf_end_device_write(const rkvc_buffer *buf);
int rkvc_rga_available(void);
int rkvc_rga_rgb888_stride(int width);
int rkvc_rga_rgb888_row_bytes(int width);
rkvc_err rkvc_rga_csc_nv12_to_rgb888(const rkvc_buffer *src, uint8_t *rgb,
                                     int w, int h, int rgb_stride_pixels);
rkvc_err rkvc_rga_csc_rgb888_to_nv12(const uint8_t *rgb, int w, int h,
                                     int rgb_stride_pixels, rkvc_buffer *dst);

typedef struct rkvc_rknn_sr_ctx rkvc_rknn_sr_ctx;

int rkvc_rknn_sr_available(void);
rkvc_rknn_sr_ctx *rkvc_rknn_sr_ctx_create(const char *model_path,
                                          int expect_out_w, int expect_out_h,
                                          rkvc_buffer_pool *pool);
void rkvc_rknn_sr_ctx_destroy(rkvc_rknn_sr_ctx *ctx);
rkvc_err rkvc_rknn_sr_ctx_process(rkvc_rknn_sr_ctx *ctx,
                                  const rkvc_buffer *src,
                                  rkvc_buffer **out);
rkvc_err rkvc_rknn_sr_ctx_submit(rkvc_rknn_sr_ctx *ctx,
                                 const rkvc_buffer *src);
rkvc_err rkvc_rknn_sr_ctx_collect(rkvc_rknn_sr_ctx *ctx,
                                  rkvc_buffer **out, int block);
int rkvc_rknn_sr_ctx_busy(const rkvc_rknn_sr_ctx *ctx);
void rkvc_rknn_sr_ctx_drain(rkvc_rknn_sr_ctx *ctx);
rkvc_err rkvc_rknn_sr_buffer(const rkvc_buffer *src, rkvc_buffer **dst,
                             int dst_w, int dst_h, const char *model_path);

typedef struct rkvc_rga_scale_ctx rkvc_rga_scale_ctx;
rkvc_rga_scale_ctx *rkvc_rga_scale_ctx_create(int dst_w, int dst_h,
                                              rkvc_upscale_algo algo);
void rkvc_rga_scale_ctx_destroy(rkvc_rga_scale_ctx *ctx);
rkvc_err rkvc_rga_scale_ctx_process(rkvc_rga_scale_ctx *ctx,
                                    const rkvc_buffer *src,
                                    rkvc_buffer **out);

/* ── Runtime quota ────────────────────────────────────────────────── */

#define RKVC_RT_FLAG_SESSION 1
#define RKVC_RT_FLAG_ENC     2
#define RKVC_RT_FLAG_NPU     4

rkvc_err rkvc_runtime_try_register(const rkvc_pipeline_desc *desc,
                                   int *out_flags);
void rkvc_runtime_unregister(int flags);

/* ── Session ──────────────────────────────────────────────────────── */

struct rkvc_session {
    rkvc_pipeline_desc  desc;
    rkvc_route_plan     route;
    rkvc_buffer_pool   *pool;
    int                 runtime_flags;

    rkvc_port           port_capture;
    rkvc_port           port_output;
    rkvc_port           port_preview;

    pthread_t           worker;
    int                 running;
    volatile int        stop_requested;
    pthread_mutex_t     lock;

    rkvc_demux           *demux;
    rkvc_mux             *mux;
    rkvc_mpp_dec         *dec;
    rkvc_mpp_enc         *enc;
    rkvc_svt_enc         *svt;
    rkvc_v4l2_cap        *v4l2;
    rkvc_rga_scale_ctx   *rga_scale;
    rkvc_rknn_sr_ctx     *rknn_sr;

    rkvc_roi_rect         rois[RKVC_ROI_MAX];
    int                   roi_count;

    unsigned              reconfig_pending; /**< RKVC_RECONFIG_* 位 */

    rkvc_session_stats    stats;
    int64_t               first_ts_us;
};

void rkvc_session_stats_tick(rkvc_session *s, int frame_out);
void rkvc_session_stats_frame_in(rkvc_session *s);
void rkvc_session_stats_drop(rkvc_session *s);
void rkvc_session_stats_add_timing(rkvc_session *s,
                                    double decode_delta,
                                    double rga_delta,
                                    double write_delta,
                                    double postproc_delta);
void rkvc_session_stats_reset_timing(rkvc_session *s);

/** 编码前应用挂起的热切换；`force_idr` 输出是否强制本帧 IDR。 */
rkvc_err rkvc_session_apply_reconfig(rkvc_session *s, int *force_idr);

#endif /* RKVC_INTERNAL_H */
