/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file utils.c
 * @brief 内部工具函数实现。
 */

#include "internal.h"
#include <errno.h>
#include <pthread.h>

/* ── Project-owned allocation wrappers ─────────────────────────────── */

#ifdef RKVC_ENABLE_FAULT_INJECTION
static pthread_mutex_t s_alloc_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static long s_alloc_fail_after = -1;

static int rkvc_should_fail_alloc(void)
{
    int should_fail = 0;

    pthread_mutex_lock(&s_alloc_fault_lock);
    if (s_alloc_fail_after >= 0) {
        if (s_alloc_fail_after == 0) {
            should_fail = 1;
        } else {
            s_alloc_fail_after--;
        }
    }
    pthread_mutex_unlock(&s_alloc_fault_lock);

    return should_fail;
}

void rkvc_test_fail_alloc_after(long countdown)
{
    pthread_mutex_lock(&s_alloc_fault_lock);
    s_alloc_fail_after = countdown < 0 ? -1 : countdown;
    pthread_mutex_unlock(&s_alloc_fault_lock);
}

void rkvc_test_clear_faults(void)
{
    pthread_mutex_lock(&s_alloc_fault_lock);
    s_alloc_fail_after = -1;
    pthread_mutex_unlock(&s_alloc_fault_lock);
}
#else
static int rkvc_should_fail_alloc(void)
{
    return 0;
}
#endif

void *rkvc_malloc(size_t size)
{
    if (rkvc_should_fail_alloc())
        return NULL;
    return malloc(size);
}

void *rkvc_calloc(size_t nmemb, size_t size)
{
    if (rkvc_should_fail_alloc())
        return NULL;
    return calloc(nmemb, size);
}

void rkvc_free(void *ptr)
{
    free(ptr);
}

/* ── Input format sniffing ─────────────────────────────────────────── */

static int annexb_start_code_at(const uint8_t *data, size_t size,
                                size_t pos, size_t *nal_offset)
{
    if (pos + 4 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x01) {
        *nal_offset = pos + 3;
        return 1;
    }

    if (pos + 5 <= size && data[pos] == 0x00 && data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x00 && data[pos + 3] == 0x01) {
        *nal_offset = pos + 4;
        return 1;
    }

    return 0;
}

static int has_annexb_start_code(const uint8_t *data, size_t size,
                                 size_t *nal_offset)
{
    return annexb_start_code_at(data, size, 0, nal_offset);
}

static int is_h264_or_h265_annexb(const uint8_t *data, size_t size)
{
    size_t nal_offset = 0;
    int h264_sps = 0;
    int h264_pps = 0;
    int h265_vps = 0;
    int h265_sps = 0;
    int h265_pps = 0;

    if (!has_annexb_start_code(data, size, &nal_offset) ||
        nal_offset >= size)
        return 0;

    for (size_t pos = 0; pos + 4 <= size; pos++) {
        if (!annexb_start_code_at(data, size, pos, &nal_offset) ||
            nal_offset >= size)
            continue;

        uint8_t h264_type = data[nal_offset] & 0x1f;
        uint8_t h265_type = (data[nal_offset] >> 1) & 0x3f;

        h264_sps |= (h264_type == 7);
        h264_pps |= (h264_type == 8);
        h265_vps |= (h265_type == 32);
        h265_sps |= (h265_type == 33);
        h265_pps |= (h265_type == 34);

        if ((h264_sps && h264_pps) ||
            (h265_vps && (h265_sps || h265_pps)) ||
            (h265_sps && h265_pps))
            return 1;

        pos = nal_offset;
    }

    return 0;
}

int rkvc_buffer_looks_compressed_video(const uint8_t *data, size_t size)
{
    if (!data || size < 4)
        return 0;

    if (is_h264_or_h265_annexb(data, size))
        return 1;

    if (size >= 12 && memcmp(data + 4, "ftyp", 4) == 0)
        return 1;

    if (size >= 4 && data[0] == 0x1a && data[1] == 0x45 &&
        data[2] == 0xdf && data[3] == 0xa3)
        return 1;

    if (size > 188 && data[0] == 0x47 && data[188] == 0x47)
        return 1;

    return 0;
}

rkvc_input_format_probe rkvc_probe_input_format(const uint8_t *data,
                                                size_t size)
{
    if (!data || size == 0)
        return RKVC_INPUT_UNKNOWN;

    if (rkvc_buffer_looks_compressed_video(data, size))
        return RKVC_INPUT_COMPRESSED_VIDEO;

    return RKVC_INPUT_UNKNOWN;
}

/* ── FFmpeg 错误码映射 ─────────────────────────────────────────────── */

rkvc_err rkvc_from_averror(int av_err)
{
    if (av_err >= 0)
        return RKVC_OK;

    switch (av_err) {
    case AVERROR(ENOMEM):     return RKVC_ERR_NOMEM;
    case AVERROR(EINVAL):     return RKVC_ERR_INVALID;
    case AVERROR(ENOENT):     return RKVC_ERR_NOT_FOUND;
    case AVERROR(EIO):        return RKVC_ERR_IO;
    case AVERROR(EACCES):     return RKVC_ERR_PERMISSION;
    case AVERROR(EPERM):      return RKVC_ERR_PERMISSION;
    case AVERROR_EOF:         return RKVC_ERR_EOF;
    case AVERROR(EAGAIN):     return RKVC_ERR_AGAIN;
    case AVERROR(ENODEV):     return RKVC_ERR_HW;
    case AVERROR_UNKNOWN:     return RKVC_ERR_INTERNAL;
    default:
        /* AVERROR_DECODER_NOT_FOUND / ENOMEM 等 */
        if (av_err == AVERROR_DECODER_NOT_FOUND ||
            av_err == AVERROR_ENCODER_NOT_FOUND)
            return RKVC_ERR_NOT_FOUND;
        return RKVC_ERR_INTERNAL;
    }
}

/* ── RKMPP 硬件设备上下文单例 ──────────────────────────────────────── */

static AVBufferRef *s_hw_device_ctx = NULL;
static pthread_mutex_t s_hw_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_hw_init_ok = 0;

rkvc_err rkvc_get_hw_device_ctx(AVBufferRef **out)
{
    if (!out)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&s_hw_lock);

    if (!s_hw_init_ok) {
        int ret = av_hwdevice_ctx_create(&s_hw_device_ctx,
                                         AV_HWDEVICE_TYPE_RKMPP,
                                         NULL, NULL, 0);
        if (ret < 0) {
            RKVC_LOG("RKMPP hw device create failed: %s", av_err2str(ret));
            s_hw_device_ctx = NULL;
            pthread_mutex_unlock(&s_hw_lock);
            return rkvc_from_averror(ret);
        }
        s_hw_init_ok = 1;
        RKVC_LOG("RKMPP hw device created");
    }

    *out = av_buffer_ref(s_hw_device_ctx);
    pthread_mutex_unlock(&s_hw_lock);

    if (!*out)
        return RKVC_ERR_NOMEM;
    return RKVC_OK;
}

int rkvc_is_valid_pix_fmt(rkvc_pix_fmt fmt)
{
    switch (fmt) {
    case RKVC_PIX_FMT_NV12:
    case RKVC_PIX_FMT_YUV420P:
    case RKVC_PIX_FMT_NV16:
    case RKVC_PIX_FMT_P010:
        return 1;
    default:
        return 0;
    }
}

int rkvc_is_valid_rc_mode(rkvc_rc_mode mode)
{
    switch (mode) {
    case RKVC_RC_CBR:
    case RKVC_RC_VBR:
    case RKVC_RC_CQP:
        return 1;
    default:
        return 0;
    }
}

/* ── 像素格式转换 ──────────────────────────────────────────────────── */

enum AVPixelFormat rkvc_to_av_pix_fmt(rkvc_pix_fmt fmt)
{
    switch (fmt) {
    case RKVC_PIX_FMT_NV12:    return AV_PIX_FMT_NV12;
    case RKVC_PIX_FMT_YUV420P: return AV_PIX_FMT_YUV420P;
    case RKVC_PIX_FMT_NV16:    return AV_PIX_FMT_NV16;
    case RKVC_PIX_FMT_P010:    return AV_PIX_FMT_P010;
    default:                    return AV_PIX_FMT_NONE;
    }
}

rkvc_pix_fmt rkvc_from_av_pix_fmt(enum AVPixelFormat fmt)
{
    switch (fmt) {
    case AV_PIX_FMT_NV12:    return RKVC_PIX_FMT_NV12;
    case AV_PIX_FMT_YUV420P: return RKVC_PIX_FMT_YUV420P;
    case AV_PIX_FMT_NV16:    return RKVC_PIX_FMT_NV16;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P010BE:  return RKVC_PIX_FMT_P010;
    /* DRM_PRIME 是 dmabuf 容器格式（实际像素格式在 AVDRMFrameDescriptor 中）；
       本项目仅消费 DRM_FORMAT_NV12 的 dmabuf（rkvc_buffer_from_drm_frame 校验），
       故显式映射为 NV12，避免落入兜底分支输出误导性告警。 */
    case AV_PIX_FMT_DRM_PRIME: return RKVC_PIX_FMT_NV12;
    default:
        rkvc_log_print(AV_LOG_WARNING,
                       "unknown AVPixelFormat %d, falling back to NV12\n",
                       (int)fmt);
        return RKVC_PIX_FMT_NV12;
    }
}

/* end utils */
