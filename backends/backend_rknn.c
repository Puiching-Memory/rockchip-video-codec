/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_rknn.c
 * @brief Phase-RLFN RKNN transform backend for the 0.4 backend ABI.
 *
 * The model is supplied by the core through bind_model(), after the
 * containing .rkmodel payload has passed its SHA-256/signature checks.  The
 * backend never re-opens a model path and never writes model bytes to disk.
 *
 * Supported contract:
 *   uint8  NHWC  1 x H x W x 12
 *       -> float NCHW 1 x 108 x H x W
 *
 * The input is NV12 at (2W x 2H).  The output is NV12 at (6W x 6H), i.e. a
 * fixed 3x upscale.  The network predicts a phase residual which is added to
 * a bicubic NV12 baseline.  Both HOST and linear DMA-BUF inputs are accepted;
 * output is tightly packed HOST memory.
 */

#include "rkvc/backend.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#endif

#include <rknn_api.h>

#define RKNN_PHASE_INPUT_FACTOR    2u
#define RKNN_PHASE_OUTPUT_FACTOR   6u
#define RKNN_PHASE_INPUT_CHANNELS  12u
#define RKNN_PHASE_OUTPUT_CHANNELS 108u
#define RKNN_MAX_DIMENSION         8192u

struct rknn_upscaler {
    rkvc_request request;
    const unsigned char *model;
    size_t model_size;
    char model_id[64];
    rknn_context ctx;
    rknn_tensor_attr input_attr;
    rknn_tensor_attr output_attr;
    uint32_t core_w, core_h;
    uint32_t input_w, input_h;
    uint32_t output_w, output_h;
    unsigned char *phase_input;
    size_t phase_input_size;
    int opened;
};

static void rknn_free_buffer(void *ptr) { free(ptr); }

static void push_reason(rkvc_diag **diag, rkvc_status status,
                        const rkvc_node *node, const char *reason) {
    if (diag)
        rkvc_diag_push(diag, status, 3,
                       node && node->ops ? node->ops->id : "rknn.upscale",
                       reason);
}

static int tensor_chw(const rknn_tensor_attr *attr, uint32_t *c,
                      uint32_t *h, uint32_t *w) {
    if (!attr || attr->n_dims != 4 || !c || !h || !w)
        return 0;
    if (attr->fmt == RKNN_TENSOR_NCHW ||
        attr->fmt == RKNN_TENSOR_UNDEFINED) {
        *c = attr->dims[1];
        *h = attr->dims[2];
        *w = attr->dims[3];
    } else if (attr->fmt == RKNN_TENSOR_NHWC) {
        *h = attr->dims[1];
        *w = attr->dims[2];
        *c = attr->dims[3];
    } else {
        return 0;
    }
    return *c > 0 && *h > 0 && *w > 0;
}

static uint8_t clip_u8(float value) {
    /* Ordered comparisons are false for NaN; clamp it deterministically. */
    if (!(value > 0.0f))
        return 0;
    if (value >= 255.0f)
        return 255;
    return (uint8_t)lrintf(value);
}

static uint8_t nv12_channel_at(const uint8_t *y_plane, uint32_t y_stride,
                               const uint8_t *uv_plane, uint32_t uv_stride,
                               uint32_t width, uint32_t height,
                               uint32_t channel, uint32_t y, uint32_t x) {
    if (channel == 0)
        return y_plane[(size_t)y * y_stride + x];

    {
        uint32_t chroma_w = width / 2;
        uint32_t chroma_h = height / 2;
        uint32_t x0 = x / 2;
        uint32_t y0 = y / 2;
        uint32_t x1 = x0 + 1 < chroma_w ? x0 + 1 : x0;
        uint32_t y1 = y0 + 1 < chroma_h ? y0 + 1 : y0;
        uint32_t offset = channel - 1;
        int a = uv_plane[(size_t)y0 * uv_stride + x0 * 2 + offset];
        int b, c, d;
        if (!(x & 1u) && !(y & 1u))
            return (uint8_t)a;
        b = uv_plane[(size_t)y0 * uv_stride + x1 * 2 + offset];
        c = uv_plane[(size_t)y1 * uv_stride + x0 * 2 + offset];
        if (!(y & 1u))
            return (uint8_t)((a + b + 1) / 2);
        if (!(x & 1u))
            return (uint8_t)((a + c + 1) / 2);
        d = uv_plane[(size_t)y1 * uv_stride + x1 * 2 + offset];
        return (uint8_t)((a + b + c + d + 2) / 4);
    }
}

static int phase_pack_nv12(const uint8_t *y_plane, uint32_t y_stride,
                           const uint8_t *uv_plane, uint32_t uv_stride,
                           uint32_t width, uint32_t height,
                           uint8_t *phases, size_t phases_size) {
    uint32_t core_w, core_h, channel, dy, dx, cy, cx;
    size_t required;
    if (!y_plane || !uv_plane || !phases || !width || !height ||
        y_stride < width || uv_stride < width ||
        width % RKNN_PHASE_INPUT_FACTOR ||
        height % RKNN_PHASE_INPUT_FACTOR)
        return -1;
    core_w = width / RKNN_PHASE_INPUT_FACTOR;
    core_h = height / RKNN_PHASE_INPUT_FACTOR;
    required = (size_t)RKNN_PHASE_INPUT_CHANNELS * core_w * core_h;
    if (phases_size < required)
        return -1;

    for (channel = 0; channel < 3; ++channel) {
        for (dy = 0; dy < RKNN_PHASE_INPUT_FACTOR; ++dy) {
            for (dx = 0; dx < RKNN_PHASE_INPUT_FACTOR; ++dx) {
                uint32_t packed_c = channel * 4 + dy * 2 + dx;
                for (cy = 0; cy < core_h; ++cy) {
                    uint32_t sy = cy * 2 + dy;
                    for (cx = 0; cx < core_w; ++cx) {
                        uint32_t sx = cx * 2 + dx;
                        size_t off = ((size_t)cy * core_w + cx) *
                                     RKNN_PHASE_INPUT_CHANNELS + packed_c;
                        phases[off] = nv12_channel_at(
                            y_plane, y_stride, uv_plane, uv_stride,
                            width, height, channel, sy, sx);
                    }
                }
            }
        }
    }
    return 0;
}

static float cubic_weight(float x) {
    const float a = -0.5f; /* Catmull-Rom */
    x = fabsf(x);
    if (x < 1.0f)
        return (a + 2.0f) * x * x * x - (a + 3.0f) * x * x + 1.0f;
    if (x < 2.0f)
        return a * x * x * x - 5.0f * a * x * x + 8.0f * a * x - 4.0f * a;
    return 0.0f;
}

static uint32_t clamp_index(int value, uint32_t limit) {
    if (value < 0)
        return 0;
    if ((uint32_t)value >= limit)
        return limit - 1;
    return (uint32_t)value;
}

/** Bicubic scale one byte channel; pixel_stride handles interleaved UV. */
static void bicubic_channel(const uint8_t *src, uint32_t src_w,
                            uint32_t src_h, uint32_t src_stride,
                            uint32_t src_pixel_stride, uint8_t *dst,
                            uint32_t dst_w, uint32_t dst_h,
                            uint32_t dst_stride, uint32_t dst_pixel_stride) {
    uint32_t y, x;
    float scale_x = (float)src_w / (float)dst_w;
    float scale_y = (float)src_h / (float)dst_h;
    for (y = 0; y < dst_h; ++y) {
        float sy = ((float)y + 0.5f) * scale_y - 0.5f;
        int iy = (int)floorf(sy);
        for (x = 0; x < dst_w; ++x) {
            float sx = ((float)x + 0.5f) * scale_x - 0.5f;
            int ix = (int)floorf(sx);
            float total = 0.0f, weights = 0.0f;
            int ky, kx;
            for (ky = -1; ky <= 2; ++ky) {
                float wy = cubic_weight(sy - (float)(iy + ky));
                uint32_t py = clamp_index(iy + ky, src_h);
                for (kx = -1; kx <= 2; ++kx) {
                    float wx = cubic_weight(sx - (float)(ix + kx));
                    uint32_t px = clamp_index(ix + kx, src_w);
                    float weight = wx * wy;
                    total += src[(size_t)py * src_stride +
                                 (size_t)px * src_pixel_stride] * weight;
                    weights += weight;
                }
            }
            if (fabsf(weights) > 1e-8f)
                total /= weights;
            dst[(size_t)y * dst_stride +
                (size_t)x * dst_pixel_stride] = clip_u8(total);
        }
    }
}

static int bicubic_nv12(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                        uint32_t src_stride, uint32_t src_vstride,
                        uint8_t *dst, uint32_t dst_w, uint32_t dst_h) {
    uint8_t *dst_uv;
    const uint8_t *src_uv;
    if (!src || !dst || !src_w || !src_h || !dst_w || !dst_h ||
        (src_w & 1u) || (src_h & 1u) || (dst_w & 1u) || (dst_h & 1u) ||
        src_stride < src_w || src_vstride < src_h)
        return -1;
    src_uv = src + (size_t)src_stride * src_vstride;
    dst_uv = dst + (size_t)dst_w * dst_h;
    bicubic_channel(src, src_w, src_h, src_stride, 1,
                    dst, dst_w, dst_h, dst_w, 1);
    bicubic_channel(src_uv, src_w / 2, src_h / 2, src_stride, 2,
                    dst_uv, dst_w / 2, dst_h / 2, dst_w, 2);
    bicubic_channel(src_uv + 1, src_w / 2, src_h / 2, src_stride, 2,
                    dst_uv + 1, dst_w / 2, dst_h / 2, dst_w, 2);
    return 0;
}

static size_t residual_offset(size_t plane, uint32_t residual_w,
                              uint32_t channel, uint32_t dy, uint32_t dx,
                              uint32_t cy, uint32_t cx) {
    uint32_t packed_c = channel * 36 + dy * 6 + dx;
    return (size_t)packed_c * plane + (size_t)cy * residual_w + cx;
}

static int add_phase_residual(const float *residual, uint32_t residual_w,
                              uint32_t residual_h, uint8_t *dst,
                              uint32_t width, uint32_t height) {
    size_t plane = (size_t)residual_w * residual_h;
    uint8_t *y_plane = dst;
    uint8_t *uv_plane = dst + (size_t)width * height;
    uint32_t cy, dy, dx, cx, ey, ex, channel;
    if (!residual || !dst || width != residual_w * 6 ||
        height != residual_h * 6 || (width & 1u) || (height & 1u))
        return -1;

    for (cy = 0; cy < residual_h; ++cy) {
        for (dy = 0; dy < 6; ++dy) {
            uint8_t *row = y_plane + (size_t)(cy * 6 + dy) * width;
            for (dx = 0; dx < 6; ++dx) {
                const float *src = residual + residual_offset(
                    plane, residual_w, 0, dy, dx, cy, 0);
                for (cx = 0; cx < residual_w; ++cx) {
                    uint8_t *pixel = row + (size_t)cx * 6 + dx;
                    *pixel = clip_u8((float)*pixel + src[cx]);
                }
            }
        }
    }
    for (cy = 0; cy < residual_h; ++cy) {
        for (ey = 0; ey < 3; ++ey) {
            uint8_t *row = uv_plane + (size_t)(cy * 3 + ey) * width;
            for (ex = 0; ex < 3; ++ex) {
                for (channel = 1; channel <= 2; ++channel) {
                    const float *s0 = residual + residual_offset(
                        plane, residual_w, channel, ey * 2, ex * 2, cy, 0);
                    const float *s1 = s0 + plane;
                    const float *s2 = s0 + 6 * plane;
                    const float *s3 = s2 + plane;
                    uint32_t offset = ex * 2 + channel - 1;
                    for (cx = 0; cx < residual_w; ++cx) {
                        uint8_t *pixel = row + (size_t)cx * 6 + offset;
                        float value = (s0[cx] + s1[cx] + s2[cx] + s3[cx])
                                      * 0.25f;
                        *pixel = clip_u8((float)*pixel + value);
                    }
                }
            }
        }
    }
    return 0;
}

static int rknn_bind_model(rkvc_node *node,
                           const rkvc_model_binding *binding,
                           rkvc_diag **diag) {
    struct rknn_upscaler *up = node ? node->priv : NULL;
    if (!up || !binding || !binding->info || !binding->payload ||
        !binding->payload_size) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "missing RKNN model binding");
        return (int)RKVC_STATUS_FORMAT;
    }
    if (strcmp(binding->info->role, "upscale") != 0) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "model role is not upscale");
        return (int)RKVC_STATUS_FORMAT;
    }
    up->model = binding->payload;
    up->model_size = binding->payload_size;
    snprintf(up->model_id, sizeof(up->model_id), "%s", binding->info->id);
    return 0;
}

static int rknn_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec input = {0};
    rkvc_frame_spec output = {0};
    (void)diag;
    /* UNKNOWN is intentional: process accepts HOST and linear DMA-BUF.
     * The concrete NV12 geometry is checked against the model later. */
    output.fmt = RKVC_FRAME_FMT_NV12;
    output.domain = RKVC_MEM_DOMAIN_HOST;
    rkvc_port_set_desired(&node->in_ports[0], &input);
    rkvc_port_set_desired(&node->out_ports[0], &output);
    return 0;
}

static int rknn_open(rkvc_node *node, rkvc_diag **diag) {
    struct rknn_upscaler *up = node->priv;
    rknn_input_output_num io_num;
    uint32_t in_c = 0, in_h = 0, in_w = 0;
    uint32_t out_c = 0, out_h = 0, out_w = 0;
    int ret;

    if (!up->model || !up->model_size || up->model_size > UINT32_MAX) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "RKNN payload is empty or too large");
        return (int)RKVC_STATUS_HW;
    }
    ret = rknn_init(&up->ctx, (void *)up->model, (uint32_t)up->model_size,
                    0, NULL);
    if (ret != RKNN_SUCC) {
        push_reason(diag, RKVC_STATUS_HW, node,
                    "rknn_init rejected the selected model/runtime pair");
        return (int)RKVC_STATUS_HW;
    }
    memset(&io_num, 0, sizeof(io_num));
    if (rknn_query(up->ctx, RKNN_QUERY_IN_OUT_NUM,
                   &io_num, sizeof(io_num)) != RKNN_SUCC ||
        io_num.n_input != 1 || io_num.n_output != 1) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "Phase-RLFN requires exactly one input and one output");
        goto incompatible;
    }
    memset(&up->input_attr, 0, sizeof(up->input_attr));
    memset(&up->output_attr, 0, sizeof(up->output_attr));
    up->input_attr.index = 0;
    up->output_attr.index = 0;
    if (rknn_query(up->ctx, RKNN_QUERY_INPUT_ATTR,
                   &up->input_attr, sizeof(up->input_attr)) != RKNN_SUCC ||
        rknn_query(up->ctx, RKNN_QUERY_OUTPUT_ATTR,
                   &up->output_attr, sizeof(up->output_attr)) != RKNN_SUCC ||
        !tensor_chw(&up->input_attr, &in_c, &in_h, &in_w) ||
        !tensor_chw(&up->output_attr, &out_c, &out_h, &out_w) ||
        up->input_attr.fmt != RKNN_TENSOR_NHWC ||
        up->input_attr.type != RKNN_TENSOR_UINT8 ||
        up->output_attr.fmt != RKNN_TENSOR_NCHW ||
        in_c != RKNN_PHASE_INPUT_CHANNELS ||
        out_c != RKNN_PHASE_OUTPUT_CHANNELS || in_w != out_w ||
        in_h != out_h || in_w > RKNN_MAX_DIMENSION / 6 ||
        in_h > RKNN_MAX_DIMENSION / 6) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "unsupported RKNN tensor contract (expected UINT8 NHWC 12-channel input and NCHW 108-channel output)");
        goto incompatible;
    }
    up->core_w = in_w;
    up->core_h = in_h;
    up->input_w = in_w * RKNN_PHASE_INPUT_FACTOR;
    up->input_h = in_h * RKNN_PHASE_INPUT_FACTOR;
    up->output_w = out_w * RKNN_PHASE_OUTPUT_FACTOR;
    up->output_h = out_h * RKNN_PHASE_OUTPUT_FACTOR;
    if ((up->request.width && up->request.width != up->input_w) ||
        (up->request.height && up->request.height != up->input_h)) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "request input geometry does not match selected model");
        goto incompatible;
    }
    up->phase_input_size = (size_t)up->input_attr.n_elems;
    if (up->phase_input_size !=
            (size_t)RKNN_PHASE_INPUT_CHANNELS * up->core_w * up->core_h ||
        (size_t)up->output_attr.n_elems !=
            (size_t)RKNN_PHASE_OUTPUT_CHANNELS * up->core_w * up->core_h) {
        push_reason(diag, RKVC_STATUS_FORMAT, node,
                    "RKNN element counts do not match tensor dimensions");
        goto incompatible;
    }
    up->phase_input = malloc(up->phase_input_size);
    if (!up->phase_input) {
        rknn_destroy(up->ctx);
        up->ctx = 0;
        return (int)RKVC_STATUS_NOMEM;
    }
    up->opened = 1;
    return 0;

incompatible:
    rknn_destroy(up->ctx);
    up->ctx = 0;
    /* open-time model incompatibility participates in the core's HW fallback. */
    return (int)RKVC_STATUS_HW;
}

#ifdef __linux__
static void dmabuf_sync(int fd, unsigned long flags) {
    struct dma_buf_sync sync = {flags};
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
#endif

static int rknn_process(rkvc_node *node, rkvc_frame *input,
                        rkvc_diag **diag) {
    struct rknn_upscaler *up = node->priv;
    rkvc_frame_desc desc;
    const uint8_t *base;
    const uint8_t *uv;
    void *mapped = NULL;
    uint8_t *output_data = NULL;
    size_t output_size;
    rknn_input rinput;
    rknn_output routput;
    rkvc_frame_desc output_desc;
    rkvc_frame *output = NULL;
    rkvc_status st;
    uint32_t stride, vstride;
    int ret, rc;

    if (!up->opened || !input ||
        rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        desc.spec.fmt != RKVC_FRAME_FMT_NV12 ||
        desc.spec.width != up->input_w || desc.spec.height != up->input_h)
        return (int)RKVC_STATUS_FORMAT;
    stride = desc.spec.stride ? desc.spec.stride : desc.spec.width;
    vstride = desc.spec.ver_stride ? desc.spec.ver_stride : desc.spec.height;
    if (stride < desc.spec.width || vstride < desc.spec.height ||
        desc.size < (size_t)stride * vstride * 3 / 2)
        return (int)RKVC_STATUS_FORMAT;

    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF) {
#ifdef __linux__
        if (desc.fd < 0 || desc.spec.modifier != 0)
            return (int)RKVC_STATUS_FORMAT;
        mapped = mmap(NULL, desc.size, PROT_READ, MAP_SHARED, desc.fd, 0);
        if (mapped == MAP_FAILED)
            return (int)RKVC_STATUS_IO;
        dmabuf_sync(desc.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        base = mapped;
#else
        return (int)RKVC_STATUS_UNSUPPORTED;
#endif
    } else {
        if (!desc.data)
            return (int)RKVC_STATUS_FORMAT;
        base = desc.data;
    }
    uv = base + (size_t)stride * vstride;
    if (phase_pack_nv12(base, stride, uv, stride, up->input_w, up->input_h,
                        up->phase_input, up->phase_input_size) != 0) {
        rc = (int)RKVC_STATUS_FORMAT;
        goto done;
    }

    memset(&rinput, 0, sizeof(rinput));
    rinput.index = 0;
    rinput.buf = up->phase_input;
    rinput.size = (uint32_t)up->phase_input_size;
    rinput.type = RKNN_TENSOR_UINT8;
    rinput.fmt = RKNN_TENSOR_NHWC;
    ret = rknn_inputs_set(up->ctx, 1, &rinput);
    if (ret == RKNN_SUCC)
        ret = rknn_run(up->ctx, NULL);
    if (ret != RKNN_SUCC) {
        push_reason(diag, RKVC_STATUS_HW, node, "RKNN inference failed");
        rc = (int)RKVC_STATUS_HW;
        goto done;
    }

    memset(&routput, 0, sizeof(routput));
    routput.index = 0;
    routput.want_float = 1;
    if (rknn_outputs_get(up->ctx, 1, &routput, NULL) != RKNN_SUCC) {
        push_reason(diag, RKVC_STATUS_HW, node, "RKNN output retrieval failed");
        rc = (int)RKVC_STATUS_HW;
        goto done;
    }
    output_size = (size_t)up->output_w * up->output_h * 3 / 2;
    output_data = malloc(output_size);
    if (!output_data) {
        rknn_outputs_release(up->ctx, 1, &routput);
        rc = (int)RKVC_STATUS_NOMEM;
        goto done;
    }
    if (!routput.buf ||
        routput.size < (size_t)up->output_attr.n_elems * sizeof(float) ||
        bicubic_nv12(base, up->input_w, up->input_h, stride, vstride,
                     output_data, up->output_w, up->output_h) != 0 ||
        add_phase_residual(routput.buf, up->core_w, up->core_h,
                           output_data, up->output_w, up->output_h) != 0) {
        rknn_outputs_release(up->ctx, 1, &routput);
        free(output_data);
        output_data = NULL;
        rc = (int)RKVC_STATUS_HW;
        goto done;
    }
    rknn_outputs_release(up->ctx, 1, &routput);

    rkvc_frame_desc_init(&output_desc, sizeof(output_desc));
    output_desc.spec.width = up->output_w;
    output_desc.spec.height = up->output_h;
    output_desc.spec.fmt = RKVC_FRAME_FMT_NV12;
    output_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    output_desc.spec.stride = up->output_w;
    output_desc.spec.ver_stride = up->output_h;
    output_desc.data = output_data;
    output_desc.size = output_size;
    output_desc.pts = desc.pts;
    output_desc.dts = desc.dts;
    output_desc.flags = desc.flags;
    st = rkvc_backend_frame_create(&output_desc, rknn_free_buffer,
                                   output_data, &output);
    if (st != RKVC_STATUS_OK) {
        free(output_data);
        output_data = NULL;
        rc = (int)st;
        goto done;
    }
    output_data = NULL;
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);

done:
    if (mapped) {
#ifdef __linux__
        dmabuf_sync(desc.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        munmap(mapped, desc.size);
#endif
    }
    return rc;
}

static void rknn_close(rkvc_node *node) {
    struct rknn_upscaler *up = node ? node->priv : NULL;
    if (!up)
        return;
    if (up->ctx)
        rknn_destroy(up->ctx);
    up->ctx = 0;
    free(up->phase_input);
    up->phase_input = NULL;
    up->phase_input_size = 0;
    up->opened = 0;
}

static void rknn_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    rknn_close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops rknn_upscale_ops = {
    "rknn.upscale", rknn_configure, rknn_open, rknn_process, NULL,
    rknn_close, rknn_destroy_node, rknn_bind_model,
};

static int rknn_matches(rkvc_operation operation, rkvc_codec codec,
                        const rkvc_device_caps *caps) {
    (void)codec;
    (void)caps;
    return operation == RKVC_OPERATION_UPSCALE;
}

static int rknn_score(const rkvc_request *request,
                      const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_REALTIME ? 100 : 220;
}

static rkvc_node *rknn_create(const rkvc_node_factory *factory,
                              const rkvc_request *request,
                              void *create_ctx) {
    rkvc_node *node = calloc(1, sizeof(*node));
    struct rknn_upscaler *up = calloc(1, sizeof(*up));
    (void)factory;
    (void)create_ctx;
    if (!node || !up) {
        free(node);
        free(up);
        return NULL;
    }
    up->request = *request;
    node->ops = &rknn_upscale_ops;
    node->priv = up;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        rknn_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "video";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = "video";
    return node;
}

static int rknn_probe(const rkvc_device_caps *caps, void *probe_ctx,
                      rkvc_diag **diag) {
    (void)caps;
    (void)probe_ctx;
    (void)diag;
#ifdef RKVC_RKNN_TESTING
    return 0;
#else
    static const char *const direct_nodes[] = {"/dev/rknpu", "/dev/rknn"};
    DIR *dir;
    struct dirent *entry;
    size_t i;
    if (access("/sys/kernel/debug/rknpu/version", R_OK) == 0)
        return 0;
    for (i = 0; i < sizeof(direct_nodes) / sizeof(direct_nodes[0]); ++i)
        if (access(direct_nodes[i], R_OK | W_OK) == 0)
            return 0;
    dir = opendir("/dev/dri/by-path");
    if (!dir)
        return -ENODEV;
    while ((entry = readdir(dir)) != NULL) {
        char path[512];
        if (!strstr(entry->d_name, "npu-render"))
            continue;
        if (snprintf(path, sizeof(path), "/dev/dri/by-path/%s",
                     entry->d_name) < (int)sizeof(path) &&
            access(path, R_OK | W_OK) == 0) {
            closedir(dir);
            return 0;
        }
    }
    closedir(dir);
    return -ENODEV;
#endif
}

static const rkvc_node_factory rknn_factories[] = {
    {
        .id = "rknn.upscale",
        .backend_id = "rknn",
        .stage = RKVC_NODE_STAGE_TRANSFORM,
        .priority = 900,
        .matches = rknn_matches,
        .score = rknn_score,
        .create = rknn_create,
    },
};

static const rkvc_node_factory *rknn_factory_list(void *probe_ctx,
                                                  size_t *count) {
    (void)probe_ctx;
    *count = sizeof(rknn_factories) / sizeof(rknn_factories[0]);
    return rknn_factories;
}

static const rkvc_backend rknn_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "rknn",
    .capability_flags = RKVC_BACKEND_CAP_RKNN,
    .probe = rknn_probe,
    .factories = rknn_factory_list,
};

const rkvc_backend *rkvc_backend_query(void) { return &rknn_backend; }
