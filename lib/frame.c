/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file frame.c
 * @brief 公共 ABI：带引用计数的媒体帧实现。
 *
 * 图内核与后端通过 rkvc_frame 交换媒体对象；帧内容由调用方持有，
 * 库仅管理引用计数与元数据，不做拷贝。
 */

#include "graph_internal.h"

#include <limits.h>

/* ── 帧生命周期 ───────────────────────────────────────────────────── */

rkvc_status rkvc_frame_wrap_host(const rkvc_frame_spec *spec, void *data,
                                 size_t size, rkvc_frame **out) {
    rkvc_frame *f;
    (void)size;

    if (!spec || !out)
        return RKVC_STATUS_INVALID;
    if (spec->fmt == RKVC_FRAME_FMT_UNKNOWN ||
        spec->domain != RKVC_MEM_DOMAIN_HOST) {
        return RKVC_STATUS_FORMAT;
    }
    /* 宿主缓冲尺寸可解码（NV12 path 足够） */
    if (data && size != SIZE_MAX && size < spec->width * spec->height * 3 / 2)
        return RKVC_STATUS_INVALID;

    f = rkvc_g_calloc(1, sizeof(*f));
    if (!f)
        return RKVC_STATUS_NOMEM;
    f->refcount = 1;
    f->spec = *spec;
    f->data = data;
    f->fd   = -1;
    f->free_fn = NULL;
    f->free_ctx = NULL;
    *out = f;
    return RKVC_STATUS_OK;
}

rkvc_frame *rkvc_frame_retain(rkvc_frame *frame) {
    if (frame)
        __atomic_add_fetch(&frame->refcount, 1, __ATOMIC_RELAXED);
    return frame;
}

void rkvc_frame_release(rkvc_frame *frame) {
    if (!frame)
        return;
    if (__atomic_sub_fetch(&frame->refcount, 1, __ATOMIC_ACQ_REL) == 0) {
        if (frame->free_fn)
            frame->free_fn(frame->free_ctx);
        rkvc_g_free(frame);
    }
}

/* ── 帧访问（只读） ───────────────────────────────────────────────── */

rkvc_status rkvc_frame_get_spec(const rkvc_frame *frame, rkvc_frame_spec *spec) {
    if (!frame || !spec)
        return RKVC_STATUS_INVALID;
    *spec = frame->spec;
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_frame_get_data(const rkvc_frame *frame, void **data, int *fd) {
    if (!frame || (!data && !fd))
        return RKVC_STATUS_INVALID;
    if (data)
        *data = frame->data;
    if (fd)
        *fd = frame->fd;
    return RKVC_STATUS_OK;
}

uint32_t rkvc_frame_ref_count(const rkvc_frame *frame) {
    return frame ? frame->refcount : 0;
}

/* ── 帧内部辅助（供图内核/测试构造） ─────────────────────────────── */
rkvc_frame *rkvc_frame_internal_alloc(const rkvc_frame_spec *spec, void *data,
                                      int fd, void (*free_fn)(void *),
                                      void *free_ctx) {
    rkvc_frame *f = rkvc_g_calloc(1, sizeof(*f));
    if (!f)
        return NULL;
    f->refcount = 1;
    f->spec = *spec;
    f->data = data;
    f->fd = fd;
    f->free_fn = free_fn;
    f->free_ctx = free_ctx;
    return f;
}
