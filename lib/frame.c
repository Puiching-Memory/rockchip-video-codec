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
#include <string.h>
#include <unistd.h>

/* ── 帧生命周期 ───────────────────────────────────────────────────── */

void rkvc_frame_desc_init(rkvc_frame_desc *desc, size_t size) {
    if (!desc || size < sizeof(rkvc_header))
        return;
    memset(desc, 0, size);
    desc->header.struct_size = size;
    desc->header.api_version = RKVC_ABI_VERSION;
    if (size >= offsetof(rkvc_frame_desc, fd) + sizeof(desc->fd))
        desc->fd = -1;
    if (size >= offsetof(rkvc_frame_desc, pts) + sizeof(desc->pts))
        desc->pts = RKVC_FRAME_TS_UNKNOWN;
    if (size >= offsetof(rkvc_frame_desc, dts) + sizeof(desc->dts))
        desc->dts = RKVC_FRAME_TS_UNKNOWN;
}

/** 校验帧描述：头部版本、struct_size 上界与域/fd 一致性。 */
static rkvc_status validate_desc(const rkvc_frame_desc *desc) {
    size_t i;

    if (!desc || desc->header.struct_size <
            offsetof(rkvc_frame_desc, fd) + sizeof(desc->fd) ||
        desc->header.struct_size > (1u << 20) ||
        (desc->header.api_version &&
         (desc->header.api_version >> 16) != RKVC_ABI_VERSION_MAJOR))
        return RKVC_STATUS_INVALID;
    if (desc->spec.fmt == RKVC_FRAME_FMT_UNKNOWN)
        return RKVC_STATUS_FORMAT;
    if (desc->spec.domain == RKVC_MEM_DOMAIN_HOST) {
        if (desc->fd >= 0)
            return RKVC_STATUS_FORMAT;
    } else if (desc->spec.domain == RKVC_MEM_DOMAIN_DMABUF) {
        if (desc->fd < 0)
            return RKVC_STATUS_FORMAT;
    } else {
        return RKVC_STATUS_FORMAT;
    }
    if (desc->header.struct_size >=
            offsetof(rkvc_frame_desc, roi_region_count) +
                sizeof(desc->roi_region_count)) {
        if (desc->roi_region_count > RKVC_ROI_MAX_REGIONS ||
            (desc->roi_region_count && !desc->roi_regions) ||
            (desc->roi_region_count &&
             desc->spec.fmt == RKVC_FRAME_FMT_BITSTREAM))
            return RKVC_STATUS_INVALID;
        for (i = 0; i < desc->roi_region_count; ++i) {
            const rkvc_roi_region *r = &desc->roi_regions[i];
            if (!r->width || !r->height || r->qp_delta < -51 ||
                r->qp_delta > 51 ||
                (desc->spec.width &&
                 (r->x >= desc->spec.width ||
                  r->width > desc->spec.width - r->x)) ||
                (desc->spec.height &&
                 (r->y >= desc->spec.height ||
                  r->height > desc->spec.height - r->y)))
                return RKVC_STATUS_INVALID;
        }
    }
    if (desc->header.struct_size >=
            offsetof(rkvc_frame_desc, encode) + sizeof(desc->encode) &&
        (desc->encode.bitrate_bps < 0 ||
         desc->encode.gop_size > INT32_MAX))
        return RKVC_STATUS_INVALID;
    return RKVC_STATUS_OK;
}

/** 按描述分配内部帧（引用计数 1）；可选字段按 struct_size 宽容读取并深拷贝 ROI。 */
static rkvc_frame *frame_from_desc(const rkvc_frame_desc *desc,
                                   void (*free_fn)(void *), void *free_ctx) {
    rkvc_frame *frame = rkvc_g_calloc(1, sizeof(*frame));
    if (!frame)
        return NULL;
    frame->refcount = 1;
    frame->spec = desc->spec;
    frame->data = desc->data;
    frame->size = desc->size;
    frame->fd = desc->fd;
    frame->pts = desc->header.struct_size >=
            offsetof(rkvc_frame_desc, pts) + sizeof(desc->pts)
            ? desc->pts : RKVC_FRAME_TS_UNKNOWN;
    frame->dts = desc->header.struct_size >=
            offsetof(rkvc_frame_desc, dts) + sizeof(desc->dts)
            ? desc->dts : RKVC_FRAME_TS_UNKNOWN;
    frame->flags = desc->header.struct_size >=
            offsetof(rkvc_frame_desc, flags) + sizeof(desc->flags)
            ? desc->flags : 0;
    if (desc->header.struct_size >=
            offsetof(rkvc_frame_desc, roi_region_count) +
                sizeof(desc->roi_region_count) &&
        desc->roi_region_count) {
        frame->roi_regions = rkvc_g_calloc(desc->roi_region_count,
                                           sizeof(*frame->roi_regions));
        if (!frame->roi_regions) {
            rkvc_g_free(frame);
            return NULL;
        }
        memcpy(frame->roi_regions, desc->roi_regions,
               desc->roi_region_count * sizeof(*frame->roi_regions));
        frame->roi_region_count = desc->roi_region_count;
    }
    if (desc->header.struct_size >=
            offsetof(rkvc_frame_desc, encode) + sizeof(desc->encode))
        frame->encode = desc->encode;
    frame->free_fn = free_fn;
    frame->free_ctx = free_ctx;
    return frame;
}

/** 计算 host 域格式所需的最小缓冲字节数（含溢出检查；码流帧恒为 0）。 */
static rkvc_status minimum_host_size(const rkvc_frame_spec *spec,
                                     size_t *required) {
    size_t row, base;
    *required = 0;
    if (spec->fmt == RKVC_FRAME_FMT_BITSTREAM ||
        !spec->width || !spec->height)
        return RKVC_STATUS_OK;
    row = spec->stride;
    if (!row) {
        size_t bytes_per_pixel =
            spec->fmt == RKVC_FRAME_FMT_P010 ? 2u :
            spec->fmt == RKVC_FRAME_FMT_RGB24 ? 3u : 1u;
        if ((size_t)spec->width > SIZE_MAX / bytes_per_pixel)
            return RKVC_STATUS_INVALID;
        row = (size_t)spec->width * bytes_per_pixel;
    }
    if (row > SIZE_MAX / spec->height)
        return RKVC_STATUS_INVALID;
    base = row * spec->height;
    switch (spec->fmt) {
    case RKVC_FRAME_FMT_NV12:
    case RKVC_FRAME_FMT_NV21:
    case RKVC_FRAME_FMT_YUV420P:
    case RKVC_FRAME_FMT_P010:
        if (base > SIZE_MAX - base / 2)
            return RKVC_STATUS_INVALID;
        *required = base + base / 2;
        break;
    case RKVC_FRAME_FMT_NV16:
        if (base > SIZE_MAX / 2)
            return RKVC_STATUS_INVALID;
        *required = base * 2;
        break;
    case RKVC_FRAME_FMT_RGB24:
        *required = base;
        break;
    default:
        return RKVC_STATUS_FORMAT;
    }
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_frame_wrap(const rkvc_frame_desc *desc, rkvc_frame **out) {
    rkvc_status st;
    if (!out)
        return RKVC_STATUS_INVALID;
    *out = NULL;
    st = validate_desc(desc);
    if (st != RKVC_STATUS_OK)
        return st;
    *out = frame_from_desc(desc, NULL, NULL);
    return *out ? RKVC_STATUS_OK : RKVC_STATUS_NOMEM;
}

rkvc_status rkvc_backend_frame_create(
    const rkvc_frame_desc *desc, rkvc_backend_frame_release_fn release_fn,
    void *release_ctx, rkvc_frame **out) {
    rkvc_status st;
    if (!out)
        return RKVC_STATUS_INVALID;
    *out = NULL;
    st = validate_desc(desc);
    if (st != RKVC_STATUS_OK)
        return st;
    *out = frame_from_desc(desc, release_fn, release_ctx);
    return *out ? RKVC_STATUS_OK : RKVC_STATUS_NOMEM;
}

/** 帧释放回调：关闭自有 fd 副本并释放承载它的堆分配。 */
static void close_owned_fd(void *ctx) {
    int *fd = ctx;
    if (fd) {
        if (*fd >= 0)
            close(*fd);
        rkvc_g_free(fd);
    }
}

rkvc_status rkvc_backend_frame_create_dmabuf(
    const rkvc_frame_desc *desc, rkvc_frame **out) {
    rkvc_frame_desc owned;
    rkvc_frame *frame;
    rkvc_status st;
    int *fd;

    if (!out)
        return RKVC_STATUS_INVALID;
    *out = NULL;
    st = validate_desc(desc);
    if (st != RKVC_STATUS_OK)
        return st;
    if (desc->spec.domain != RKVC_MEM_DOMAIN_DMABUF)
        return RKVC_STATUS_FORMAT;
    fd = rkvc_g_calloc(1, sizeof(*fd));
    if (!fd)
        return RKVC_STATUS_NOMEM;
    *fd = dup(desc->fd);
    if (*fd < 0) {
        rkvc_g_free(fd);
        return RKVC_STATUS_IO;
    }
    owned = *desc;
    owned.fd = *fd;
    frame = frame_from_desc(&owned, close_owned_fd, fd);
    if (!frame) {
        close_owned_fd(fd);
        return RKVC_STATUS_NOMEM;
    }
    *out = frame;
    return RKVC_STATUS_OK;
}

rkvc_status rkvc_frame_wrap_host(const rkvc_frame_spec *spec, void *data,
                                 size_t size, rkvc_frame **out) {
    rkvc_frame_desc desc;
    size_t required;
    rkvc_status st;

    if (!spec || !out)
        return RKVC_STATUS_INVALID;
    *out = NULL;
    if (spec->fmt == RKVC_FRAME_FMT_UNKNOWN ||
        spec->domain != RKVC_MEM_DOMAIN_HOST) {
        return RKVC_STATUS_FORMAT;
    }
    st = minimum_host_size(spec, &required);
    if (st != RKVC_STATUS_OK)
        return st;
    if (data && size != SIZE_MAX && size < required)
        return RKVC_STATUS_INVALID;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec = *spec;
    desc.data = data;
    desc.size = size == SIZE_MAX ? 0 : size;
    return rkvc_frame_wrap(&desc, out);
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
        rkvc_g_free(frame->roi_regions);
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

rkvc_status rkvc_frame_get_desc(const rkvc_frame *frame,
                                rkvc_frame_desc *desc) {
    if (!frame || !desc)
        return RKVC_STATUS_INVALID;
    rkvc_frame_desc_init(desc, sizeof(*desc));
    desc->spec = frame->spec;
    desc->data = frame->data;
    desc->size = frame->size;
    desc->fd = frame->fd;
    desc->pts = frame->pts;
    desc->dts = frame->dts;
    desc->flags = frame->flags;
    desc->roi_regions = frame->roi_regions;
    desc->roi_region_count = frame->roi_region_count;
    desc->encode = frame->encode;
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
    f->size = 0;
    f->fd = fd;
    f->pts = RKVC_FRAME_TS_UNKNOWN;
    f->dts = RKVC_FRAME_TS_UNKNOWN;
    f->free_fn = free_fn;
    f->free_ctx = free_ctx;
    return f;
}
