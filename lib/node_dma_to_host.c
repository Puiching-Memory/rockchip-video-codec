/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_dma_to_host.c
 * @brief DMA-BUF / DRM 帧下载到主机 NV12（SVT 支路）。
 */

#include "internal.h"

rkvc_err rkvc_dma_to_host(const rkvc_buffer *src, rkvc_buffer **dst)
{
    if (!src || !dst || src->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;

    *dst = NULL;

    if (src->mem_type == RKVC_MEM_HOST && src->av_frame) {
        *dst = rkvc_buffer_ref((rkvc_buffer *)src);
        return RKVC_OK;
    }

    /*
     * dma-heap mmap 缓冲已在 CPU 可见（RGA 输出等），无需 hwframe 下载。
     */
    if (src->mem_type == RKVC_MEM_DMABUF && src->av_frame &&
        src->av_frame->format != AV_PIX_FMT_DRM_PRIME &&
        src->av_frame->data[0]) {
        *dst = rkvc_buffer_ref((rkvc_buffer *)src);
        return RKVC_OK;
    }

    if (!src->av_frame || src->av_frame->format != AV_PIX_FMT_DRM_PRIME)
        return RKVC_ERR_FORMAT;

    AVFrame *sw = av_frame_alloc();
    if (!sw)
        return RKVC_ERR_NOMEM;

    sw->format = AV_PIX_FMT_NV12;
    int ret = av_hwframe_transfer_data(sw, src->av_frame, 0);
    if (ret < 0) {
        av_frame_free(&sw);
        return rkvc_from_averror(ret);
    }

    sw->pts = src->pts;
    *dst = rkvc_buffer_wrap_avframe(sw, 1);
    if (!*dst) {
        av_frame_free(&sw);
        return RKVC_ERR_NOMEM;
    }
    (*dst)->format = RKVC_PIX_FMT_NV12;
    return RKVC_OK;
}
