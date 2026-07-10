/**
 * @file node_v4l2.c
 * @brief V4L2 采集节点（NV12 / MPLANE）；`device=="mock"` 时走合成帧，无需摄像头。
 */

#include "internal.h"

#include <errno.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#ifndef V4L2_PIX_FMT_NV12
#define V4L2_PIX_FMT_NV12 v4l2_fourcc('N', 'V', '1', '2')
#endif

#define RKVC_V4L2_NBUF 4

struct rkvc_v4l2_buf {
    void   *start;
    size_t  length;
};

struct rkvc_v4l2_cap {
    int                 fd;
    int                 width;
    int                 height;
    int                 stride; /**< 驱动 bytesperline（Y/UV 行跨度），≥ width */
    int                 fps_num;
    int                 fps_den;
    int                 streaming;
    int                 mock; /**< 非 0：合成 NV12，不访问真实设备 */
    int                 nbuf;
    int                 mplane; /**< 1=MPLANE，0=单平面 */
    struct rkvc_v4l2_buf bufs[RKVC_V4L2_NBUF];
    int64_t             next_pts;
    char                device[256];
};

static int is_mock_device(const char *device)
{
    return device && (strcmp(device, "mock") == 0 ||
                      strncmp(device, "mock:", 5) == 0);
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static void v4l2_unmap(rkvc_v4l2_cap *c)
{
    for (int i = 0; i < c->nbuf; i++) {
        if (c->bufs[i].start && c->bufs[i].start != MAP_FAILED)
            munmap(c->bufs[i].start, c->bufs[i].length);
        c->bufs[i].start = NULL;
        c->bufs[i].length = 0;
    }
    c->nbuf = 0;
}

static rkvc_err mock_fill_nv12(rkvc_buffer *frame, int w, int h, int64_t pts)
{
    uint8_t *planes[4] = {0};
    int strides[4] = {0};
    rkvc_err err = rkvc_buffer_get_video_planes(frame, planes, strides);
    if (err != RKVC_OK)
        return err;

    /* 竖直灰阶条 + 随 pts 水平滚动，便于肉眼/哈希区分帧 */
    int shift = (int)(pts % (int64_t)(w > 0 ? w : 1));
    for (int y = 0; y < h; y++) {
        uint8_t *row = planes[0] + y * strides[0];
        for (int x = 0; x < w; x++)
            row[x] = (uint8_t)(((x + shift) * 255) / (w > 1 ? w - 1 : 1));
    }
    for (int y = 0; y < h / 2; y++) {
        uint8_t *row = planes[1] + y * strides[1];
        for (int x = 0; x < w; x += 2) {
            row[x] = 128;
            if (x + 1 < w)
                row[x + 1] = 128;
        }
    }
    return RKVC_OK;
}

static rkvc_err mock_read_frame(rkvc_v4l2_cap *c, rkvc_buffer **out)
{
    rkvc_buffer *frame = NULL;
    rkvc_err err =
        rkvc_buffer_alloc_video_host(&frame, c->width, c->height,
                                     RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK)
        return err;

    err = mock_fill_nv12(frame, c->width, c->height, c->next_pts);
    if (err != RKVC_OK) {
        rkvc_buffer_unref(frame);
        return err;
    }
    rkvc_buffer_set_pts(frame, c->next_pts++);
    *out = frame;
    return RKVC_OK;
}

rkvc_err rkvc_v4l2_open(rkvc_v4l2_cap **out, const rkvc_v4l2_config *cfg)
{
    if (!out || !cfg || !cfg->device || !cfg->device[0])
        return RKVC_ERR_INVALID;
    if (cfg->width <= 0 || cfg->height <= 0)
        return RKVC_ERR_INVALID;

    *out = NULL;
    rkvc_v4l2_cap *c = rkvc_calloc(1, sizeof(*c));
    if (!c)
        return RKVC_ERR_NOMEM;

    snprintf(c->device, sizeof(c->device), "%s", cfg->device);
    c->fd = -1;
    c->width = cfg->width;
    c->height = cfg->height;
    c->stride = cfg->width;
    c->fps_num = cfg->fps_num > 0 ? cfg->fps_num : 30;
    c->fps_den = cfg->fps_den > 0 ? cfg->fps_den : 1;

    if (is_mock_device(c->device)) {
        c->mock = 1;
        c->streaming = 1;
        c->stride = c->width;
        RKVC_LOG("v4l2 open mock %dx%d (synthetic NV12)", c->width, c->height);
        *out = c;
        return RKVC_OK;
    }

    c->fd = open(c->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (c->fd < 0) {
        rkvc_free(c);
        return (errno == EACCES || errno == EPERM) ? RKVC_ERR_PERMISSION
                                                   : RKVC_ERR_IO;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(c->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        rkvc_v4l2_close(c);
        return RKVC_ERR_IO;
    }

    int mplane = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE) != 0;
    int single = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) != 0;
    if (!mplane && !single) {
        rkvc_v4l2_close(c);
        return RKVC_ERR_FORMAT;
    }
    c->mplane = mplane ? 1 : 0;

    enum v4l2_buf_type type =
        mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
               : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (mplane) {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        fmt.fmt.pix_mp.width = (uint32_t)c->width;
        fmt.fmt.pix_mp.height = (uint32_t)c->height;
        fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        fmt.fmt.pix_mp.num_planes = 1;
        if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
            rkvc_v4l2_close(c);
            return RKVC_ERR_FORMAT;
        }
        c->width = (int)fmt.fmt.pix_mp.width;
        c->height = (int)fmt.fmt.pix_mp.height;
        c->stride = (int)fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        if (c->stride < c->width)
            c->stride = c->width;
    } else {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        fmt.fmt.pix.width = (uint32_t)c->width;
        fmt.fmt.pix.height = (uint32_t)c->height;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
            rkvc_v4l2_close(c);
            return RKVC_ERR_FORMAT;
        }
        c->width = (int)fmt.fmt.pix.width;
        c->height = (int)fmt.fmt.pix.height;
        c->stride = (int)fmt.fmt.pix.bytesperline;
        if (c->stride < c->width)
            c->stride = c->width;
    }

    struct v4l2_streamparm parm;
    memset(&parm, 0, sizeof(parm));
    parm.type = type;
    parm.parm.capture.timeperframe.numerator = (uint32_t)c->fps_den;
    parm.parm.capture.timeperframe.denominator = (uint32_t)c->fps_num;
    (void)xioctl(c->fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = RKVC_V4L2_NBUF;
    req.type = type;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 1) {
        rkvc_v4l2_close(c);
        return RKVC_ERR_IO;
    }
    c->nbuf = (int)req.count;
    if (c->nbuf > RKVC_V4L2_NBUF)
        c->nbuf = RKVC_V4L2_NBUF;

    for (int i = 0; i < c->nbuf; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)i;
        if (mplane) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            rkvc_v4l2_close(c);
            return RKVC_ERR_IO;
        }
        size_t length = mplane ? planes[0].length : buf.length;
        off_t offset = mplane ? (off_t)planes[0].m.mem_offset
                              : (off_t)buf.m.offset;
        c->bufs[i].length = length;
        c->bufs[i].start =
            mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd,
                 offset);
        if (c->bufs[i].start == MAP_FAILED) {
            rkvc_v4l2_close(c);
            return RKVC_ERR_IO;
        }
    }

    for (int i = 0; i < c->nbuf; i++) {
        struct v4l2_buffer buf;
        struct v4l2_plane planes[1];
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)i;
        if (mplane) {
            buf.m.planes = planes;
            buf.length = 1;
        }
        if (xioctl(c->fd, VIDIOC_QBUF, &buf) < 0) {
            rkvc_v4l2_close(c);
            return RKVC_ERR_IO;
        }
    }

    if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
        RKVC_LOG("v4l2 STREAMON failed on %s: errno=%d (%s)", c->device, errno,
                 strerror(errno));
        rkvc_v4l2_close(c);
        return RKVC_ERR_IO;
    }
    c->streaming = 1;

    RKVC_LOG("v4l2 open %s %dx%d stride=%d NV12 nbuf=%d", c->device, c->width,
             c->height, c->stride, c->nbuf);
    *out = c;
    return RKVC_OK;
}

void rkvc_v4l2_close(rkvc_v4l2_cap *c)
{
    if (!c)
        return;
    if (c->mock) {
        rkvc_free(c);
        return;
    }
    if (c->fd >= 0 && c->streaming) {
        enum v4l2_buf_type type = c->mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                            : V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)xioctl(c->fd, VIDIOC_STREAMOFF, &type);
        c->streaming = 0;
    }
    v4l2_unmap(c);
    if (c->fd >= 0)
        close(c->fd);
    rkvc_free(c);
}

void rkvc_v4l2_get_size(const rkvc_v4l2_cap *c, int *w, int *h)
{
    if (w)
        *w = c ? c->width : 0;
    if (h)
        *h = c ? c->height : 0;
}

rkvc_err rkvc_v4l2_read_frame(rkvc_v4l2_cap *c, rkvc_buffer **out,
                              int timeout_ms)
{
    if (!c || !out)
        return RKVC_ERR_INVALID;
    *out = NULL;

    if (c->mock)
        return mock_read_frame(c, out);

    if (c->fd < 0)
        return RKVC_ERR_INVALID;

    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr == 0)
        return RKVC_ERR_AGAIN;
    if (pr < 0)
        return RKVC_ERR_IO;

    enum v4l2_buf_type type =
        c->mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                  : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    memset(&buf, 0, sizeof(buf));
    memset(planes, 0, sizeof(planes));
    buf.type = type;
    buf.memory = V4L2_MEMORY_MMAP;
    if (c->mplane) {
        buf.m.planes = planes;
        buf.length = 1;
    }
    if (xioctl(c->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN)
            return RKVC_ERR_AGAIN;
        return RKVC_ERR_IO;
    }

    if (buf.index >= (uint32_t)c->nbuf) {
        (void)xioctl(c->fd, VIDIOC_QBUF, &buf);
        return RKVC_ERR_INTERNAL;
    }

    const int w = c->width;
    const int h = c->height;
    const int src_stride = c->stride > 0 ? c->stride : w;
    size_t bytesused = c->mplane ? planes[0].bytesused : buf.bytesused;
    /* 至少需要 stride*h + stride*(h/2) 的映射长度 */
    size_t need_map = (size_t)src_stride * (size_t)h +
                      (size_t)src_stride * ((size_t)h / 2);
    if (c->bufs[buf.index].length < need_map && bytesused < need_map) {
        (void)xioctl(c->fd, VIDIOC_QBUF, &buf);
        return RKVC_ERR_FORMAT;
    }

    rkvc_buffer *frame = NULL;
    rkvc_err err =
        rkvc_buffer_alloc_video_host(&frame, c->width, c->height,
                                     RKVC_PIX_FMT_NV12);
    if (err != RKVC_OK) {
        (void)xioctl(c->fd, VIDIOC_QBUF, &buf);
        return err;
    }

    uint8_t *planes_out[4] = {0};
    int strides[4] = {0};
    err = rkvc_buffer_get_video_planes(frame, planes_out, strides);
    if (err != RKVC_OK) {
        rkvc_buffer_unref(frame);
        (void)xioctl(c->fd, VIDIOC_QBUF, &buf);
        return err;
    }

    const uint8_t *src = (const uint8_t *)c->bufs[buf.index].start;
    const uint8_t *uv = src + (size_t)src_stride * (size_t)h;

    if (src_stride == w && strides[0] == w && strides[1] == w) {
        memcpy(planes_out[0], src, (size_t)w * (size_t)h);
        memcpy(planes_out[1], uv, (size_t)w * (size_t)h / 2);
    } else {
        for (int y = 0; y < h; y++)
            memcpy(planes_out[0] + y * strides[0],
                   src + (size_t)y * (size_t)src_stride, (size_t)w);
        for (int y = 0; y < h / 2; y++)
            memcpy(planes_out[1] + y * strides[1],
                   uv + (size_t)y * (size_t)src_stride, (size_t)w);
    }

    rkvc_buffer_set_pts(frame, c->next_pts++);
    if (xioctl(c->fd, VIDIOC_QBUF, &buf) < 0) {
        rkvc_buffer_unref(frame);
        return RKVC_ERR_IO;
    }

    *out = frame;
    return RKVC_OK;
}
