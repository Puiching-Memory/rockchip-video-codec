/**
 * @file buffer_pool.c
 * @brief rkvc_buffer 引用计数、主机/DMA 分配与池化。
 */

#include "internal.h"

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <drm/drm_fourcc.h>

#ifdef __linux__
#include <linux/dma-buf.h>
#endif

#ifndef DMA_HEAP_IOCTL_ALLOC
struct dma_heap_allocation_data {
    uint64_t len;
    uint32_t fd;
    uint32_t fd_flags;
    uint64_t heap_flags;
};
#define DMA_HEAP_IOCTL_ALLOC \
    _IOWR('H', 0x0, struct dma_heap_allocation_data)
#endif

struct rkvc_buffer_pool {
    pthread_mutex_t lock;
};

static void buffer_init_lock(rkvc_buffer *b)
{
    pthread_mutex_init(&b->lock, NULL);
    b->ref_count = 1;
}

rkvc_buffer_pool *rkvc_buffer_pool_create(void)
{
    rkvc_buffer_pool *pool = rkvc_calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;
    pthread_mutex_init(&pool->lock, NULL);
    return pool;
}

void rkvc_buffer_pool_destroy(rkvc_buffer_pool *pool)
{
    if (!pool)
        return;
    pthread_mutex_destroy(&pool->lock);
    rkvc_free(pool);
}

static int dma_heap_open(void)
{
    static const char *const heaps[] = {
        "/dev/dma_heap/system-uncached",
        "/dev/dma_heap/system",
        NULL
    };

    for (int i = 0; heaps[i]; i++) {
        int fd = open(heaps[i], O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            return fd;
    }
    return -1;
}

static int dma_heap_alloc(int size)
{
    int heap_fd = dma_heap_open();
    if (heap_fd < 0)
        return -1;

    struct dma_heap_allocation_data alloc = {0};
    alloc.len = (uint64_t)size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;

    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    return (int)alloc.fd;
}

rkvc_err rkvc_avframe_alloc_contiguous(AVFrame *av_frame)
{
    if (!av_frame || av_frame->width <= 0 || av_frame->height <= 0)
        return RKVC_ERR_INVALID;

    enum AVPixelFormat av_fmt = (enum AVPixelFormat)av_frame->format;
    if (av_fmt == AV_PIX_FMT_NONE)
        return RKVC_ERR_INVALID;

    int buf_size = av_image_get_buffer_size(av_fmt,
                                            av_frame->width,
                                            av_frame->height, 1);
    if (buf_size < 0)
        return rkvc_from_averror(buf_size);

    AVBufferRef *buf = av_buffer_alloc(buf_size);
    if (!buf)
        return RKVC_ERR_NOMEM;

    int ret = av_image_fill_arrays(av_frame->data, av_frame->linesize,
                                   buf->data, av_fmt,
                                   av_frame->width, av_frame->height, 1);
    if (ret < 0) {
        av_buffer_unref(&buf);
        return rkvc_from_averror(ret);
    }

    av_frame->buf[0] = buf;
    av_frame->extended_data = av_frame->data;
    return RKVC_OK;
}

static rkvc_buffer *buffer_new_video(void)
{
    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->kind = RKVC_BUF_VIDEO;
    buffer_init_lock(b);
    return b;
}

rkvc_err rkvc_buffer_alloc_video_host(rkvc_buffer **out,
                                      int width, int height,
                                      rkvc_pix_fmt format)
{
    if (!out || width <= 0 || height <= 0 || !rkvc_is_valid_pix_fmt(format))
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_buffer *b = buffer_new_video();
    if (!b)
        return RKVC_ERR_NOMEM;

    b->mem_type = RKVC_MEM_HOST;
    b->fd       = -1;
    b->width    = (uint32_t)width;
    b->height   = (uint32_t)height;
    b->format   = format;
    b->owns_avframe = 1;

    b->av_frame = av_frame_alloc();
    if (!b->av_frame) {
        rkvc_buffer_unref(b);
        return RKVC_ERR_NOMEM;
    }

    b->av_frame->width  = width;
    b->av_frame->height = height;
    b->av_frame->format = rkvc_to_av_pix_fmt(format);

    rkvc_err err = rkvc_avframe_alloc_contiguous(b->av_frame);
    if (err != RKVC_OK) {
        rkvc_buffer_unref(b);
        return err;
    }

    for (int i = 0; i < 4; i++) {
        b->strides[i] = (uint32_t)b->av_frame->linesize[i];
    }

    *out = b;
    return RKVC_OK;
}

rkvc_err rkvc_buffer_pool_alloc_video(rkvc_buffer_pool *pool,
                                      rkvc_buffer **out,
                                      int width, int height,
                                      rkvc_pix_fmt format,
                                      rkvc_mem_type mem_type)
{
    (void)pool;

    if (mem_type == RKVC_MEM_DMABUF) {
        enum AVPixelFormat av_fmt = rkvc_to_av_pix_fmt(format);
        int size = av_image_get_buffer_size(av_fmt, width, height, 1);
        if (size < 0)
            return rkvc_from_averror(size);

        int fd = dma_heap_alloc(size);
        if (fd < 0)
            return rkvc_buffer_alloc_video_host(out, width, height, format);

        void *map = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            return rkvc_buffer_alloc_video_host(out, width, height, format);
        }

        rkvc_buffer *b = buffer_new_video();
        if (!b) {
            munmap(map, (size_t)size);
            close(fd);
            return RKVC_ERR_NOMEM;
        }

        b->mem_type = RKVC_MEM_DMABUF;
        b->fd       = fd;
        b->width    = (uint32_t)width;
        b->height   = (uint32_t)height;
        b->format   = format;
        b->owns_avframe = 1;

        b->av_frame = av_frame_alloc();
        if (!b->av_frame) {
            rkvc_buffer_unref(b);
            munmap(map, (size_t)size);
            close(fd);
            return RKVC_ERR_NOMEM;
        }

        b->av_frame->width  = width;
        b->av_frame->height = height;
        b->av_frame->format = av_fmt;

        int ret = av_image_fill_arrays(b->av_frame->data, b->av_frame->linesize,
                                       map, av_fmt, width, height, 1);
        if (ret < 0) {
            rkvc_buffer_unref(b);
            munmap(map, (size_t)size);
            close(fd);
            return rkvc_from_averror(ret);
        }

        for (int i = 0; i < 4; i++)
            b->strides[i] = (uint32_t)b->av_frame->linesize[i];

        *out = b;
        return RKVC_OK;
    }

    return rkvc_buffer_alloc_video_host(out, width, height, format);
}

rkvc_buffer *rkvc_buffer_wrap_avframe(AVFrame *frame, int take_ownership)
{
    if (!frame)
        return NULL;

    rkvc_buffer *b = buffer_new_video();
    if (!b)
        return NULL;

    b->mem_type     = RKVC_MEM_HOST;
    b->fd           = -1;
    b->width        = (uint32_t)frame->width;
    b->height       = (uint32_t)frame->height;
    b->format       = rkvc_from_av_pix_fmt((enum AVPixelFormat)frame->format);
    b->pts          = frame->pts;
    b->av_frame     = frame;
    b->owns_avframe = take_ownership;

    if (frame->format == AV_PIX_FMT_DRM_PRIME)
        b->mem_type = RKVC_MEM_DMABUF;

    for (int i = 0; i < 4; i++)
        b->strides[i] = (uint32_t)frame->linesize[i];

    return b;
}

rkvc_buffer *rkvc_buffer_from_drm_frame(AVFrame *hw_frame)
{
    if (!hw_frame || hw_frame->format != AV_PIX_FMT_DRM_PRIME)
        return NULL;

    AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)hw_frame->data[0];
    if (!desc || desc->nb_objects == 0 || desc->nb_layers == 0)
        return NULL;

    const AVDRMLayerDescriptor *layer = &desc->layers[0];
    /* 线性 NV12：双 plane；AFBC/RFBC 为单 plane，需 hw download。 */
    if (layer->format != DRM_FORMAT_NV12 || layer->nb_planes < 2)
        return NULL;

    const int pitch = layer->planes[0].pitch;
    if (pitch <= 0)
        return NULL;

    rkvc_buffer *b = rkvc_buffer_wrap_avframe(hw_frame, 1);
    if (!b)
        return NULL;

    b->mem_type = RKVC_MEM_DMABUF;
    b->fd       = desc->objects[0].fd;
    b->modifier = desc->objects[0].format_modifier;
    b->format   = RKVC_PIX_FMT_NV12;
    b->width    = (uint32_t)hw_frame->width;
    b->height   = (uint32_t)hw_frame->height;
    b->strides[0] = (uint32_t)pitch;
    b->strides[1] = (uint32_t)pitch;
    if (b->av_frame) {
        b->av_frame->linesize[0] = pitch;
        b->av_frame->linesize[1] = pitch;
    }
    return b;
}

rkvc_err rkvc_buffer_alloc_bitstream(rkvc_buffer **out,
                                     const uint8_t *data, size_t size,
                                     int copy)
{
    if (!out || !data || size == 0)
        return RKVC_ERR_INVALID;

    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b)
        return RKVC_ERR_NOMEM;

    b->kind = RKVC_BUF_BITSTREAM;
    buffer_init_lock(b);

    if (copy) {
        b->data = rkvc_malloc(size);
        if (!b->data) {
            rkvc_buffer_unref(b);
            return RKVC_ERR_NOMEM;
        }
        memcpy(b->data, data, size);
        b->owns_data = 1;
    } else {
        b->data = (uint8_t *)data;
        b->owns_data = 0;
    }

    b->size = size;
    *out = b;
    return RKVC_OK;
}

rkvc_buffer *rkvc_buffer_from_avpacket(const AVPacket *avpkt)
{
    if (!avpkt || avpkt->size <= 0)
        return NULL;

    rkvc_buffer *b = rkvc_calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    b->kind = RKVC_BUF_BITSTREAM;
    buffer_init_lock(b);

    b->data = rkvc_malloc((size_t)avpkt->size);
    if (!b->data) {
        rkvc_buffer_unref(b);
        return NULL;
    }
    memcpy(b->data, avpkt->data, (size_t)avpkt->size);
    b->size      = (size_t)avpkt->size;
    b->owns_data = 1;
    b->pts       = avpkt->pts;
    b->dts       = avpkt->dts;
    b->key_frame = (avpkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;

    return b;
}

rkvc_err rkvc_buffer_to_host_frame(rkvc_buffer *buf, AVFrame **frame_out)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO || !frame_out)
        return RKVC_ERR_INVALID;

    if (buf->mem_type == RKVC_MEM_DMABUF) {
        rkvc_buffer *host = NULL;
        rkvc_err err = rkvc_dma_to_host(buf, &host);
        if (err != RKVC_OK)
            return err;
        *frame_out = av_frame_clone(host->av_frame);
        rkvc_buffer_unref(host);
        if (!*frame_out)
            return RKVC_ERR_NOMEM;
        return RKVC_OK;
    }

    if (!buf->av_frame)
        return RKVC_ERR_INVALID;

    *frame_out = av_frame_clone(buf->av_frame);
    if (!*frame_out)
        return RKVC_ERR_NOMEM;
    return RKVC_OK;
}

rkvc_buffer *rkvc_buffer_ref(rkvc_buffer *buf)
{
    if (!buf)
        return NULL;
    pthread_mutex_lock(&buf->lock);
    buf->ref_count++;
    pthread_mutex_unlock(&buf->lock);
    return buf;
}

static void buffer_free(rkvc_buffer *b)
{
    if (b->kind == RKVC_BUF_VIDEO) {
        int close_fd = -1;
        if (b->owns_avframe && b->av_frame) {
            /*
             * RKMPP DRM 帧的 fd 由 AVFrame/MPP 池管理，av_frame_free 会归还；
             * 仅对 dma-heap 自分配缓冲（NV12 等）在释放后 close fd。
             */
            if (b->mem_type == RKVC_MEM_DMABUF && b->fd >= 0 &&
                b->av_frame->format != AV_PIX_FMT_DRM_PRIME)
                close_fd = b->fd;
            av_frame_free(&b->av_frame);
            if (close_fd >= 0)
                close(close_fd);
        }
    } else if (b->kind == RKVC_BUF_BITSTREAM) {
        if (b->owns_data)
            rkvc_free(b->data);
    }
    pthread_mutex_destroy(&b->lock);
    rkvc_free(b);
}

void rkvc_buffer_unref(rkvc_buffer *buf)
{
    if (!buf)
        return;

    pthread_mutex_lock(&buf->lock);
    int dead = (--buf->ref_count <= 0);
    pthread_mutex_unlock(&buf->lock);

    if (dead)
        buffer_free(buf);
}

rkvc_buffer_kind rkvc_buffer_kind_of(const rkvc_buffer *buf)
{
    return buf ? buf->kind : RKVC_BUF_NONE;
}

rkvc_err rkvc_buffer_get_video_info(const rkvc_buffer *buf,
                                    rkvc_buffer_video_info *info)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO || !info)
        return RKVC_ERR_INVALID;

    info->mem_type = buf->mem_type;
    info->fd       = buf->fd;
    info->width    = buf->width;
    info->height   = buf->height;
    info->format   = buf->format;
    info->pts      = buf->pts;
    info->modifier = buf->modifier;
    memcpy(info->strides, buf->strides, sizeof(info->strides));
    return RKVC_OK;
}

rkvc_err rkvc_buffer_get_video_planes(rkvc_buffer *buf,
                                      uint8_t *planes[4],
                                      int strides[4])
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO || !planes || !strides)
        return RKVC_ERR_INVALID;

    if (!buf->av_frame)
        return RKVC_ERR_INVALID;

    for (int i = 0; i < 4; i++) {
        planes[i]  = buf->av_frame->data[i];
        strides[i] = buf->av_frame->linesize[i];
    }
    return RKVC_OK;
}

rkvc_err rkvc_buffer_get_bitstream(const rkvc_buffer *buf,
                                   rkvc_buffer_bitstream_view *view)
{
    if (!buf || buf->kind != RKVC_BUF_BITSTREAM || !view)
        return RKVC_ERR_INVALID;

    view->data      = buf->data;
    view->size      = buf->size;
    view->pts       = buf->pts;
    view->dts       = buf->dts;
    view->key_frame = buf->key_frame;
    return RKVC_OK;
}

rkvc_err rkvc_buffer_set_pts(rkvc_buffer *buf, int64_t pts)
{
    if (!buf)
        return RKVC_ERR_INVALID;
    buf->pts = pts;
    if (buf->av_frame)
        buf->av_frame->pts = pts;
    return RKVC_OK;
}

rkvc_err rkvc_buffer_dmabuf_begin_cpu_read(const rkvc_buffer *buf)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;
    if (buf->mem_type != RKVC_MEM_DMABUF || buf->fd < 0)
        return RKVC_OK;
#ifdef __linux__
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ,
    };
    if (ioctl(buf->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return RKVC_ERR_IO;
#endif
    return RKVC_OK;
}

rkvc_err rkvc_buffer_dmabuf_end_cpu_read(const rkvc_buffer *buf)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;
    if (buf->mem_type != RKVC_MEM_DMABUF || buf->fd < 0)
        return RKVC_OK;
#ifdef __linux__
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ,
    };
    if (ioctl(buf->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return RKVC_ERR_IO;
#endif
    return RKVC_OK;
}

rkvc_err rkvc_buffer_dmabuf_begin_device_write(const rkvc_buffer *buf)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;
    if (buf->mem_type != RKVC_MEM_DMABUF || buf->fd < 0)
        return RKVC_OK;
#ifdef __linux__
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE,
    };
    if (ioctl(buf->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return RKVC_ERR_IO;
#endif
    return RKVC_OK;
}

rkvc_err rkvc_buffer_dmabuf_end_device_write(const rkvc_buffer *buf)
{
    if (!buf || buf->kind != RKVC_BUF_VIDEO)
        return RKVC_ERR_INVALID;
    if (buf->mem_type != RKVC_MEM_DMABUF || buf->fd < 0)
        return RKVC_OK;
#ifdef __linux__
    struct dma_buf_sync sync = {
        .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE,
    };
    if (ioctl(buf->fd, DMA_BUF_IOCTL_SYNC, &sync) < 0)
        return RKVC_ERR_IO;
#endif
    return RKVC_OK;
}
