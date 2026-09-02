/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_rga.c
 * @brief 面向版本化 rkvc ABI 的自足 Rockchip RGA 后端：帧缩放/搬运。
 *
 * rga.scale（TRANSFORM 阶段）：接受上游 NV12/NV21/YUV420P/RGB24 帧
 * （HOST 指针或 DMA-BUF fd），经 RGA 硬件 bicubic 缩放到目标几何并输出
 * 线性 HOST 帧。TRANSCODE 显式改尺寸取请求 width/height；UPSCALE 恒按
 * 2× 放大（其 width/height 为输入几何，供 file.source 按帧切分），作为
 * 无 NPU/无模型场景的回退路径。
 *
 * 该节点逐帧无状态：configure 只声明端口偏好，open 验证 RGA 设备存在，
 * process 每帧独立分配输出缓冲（释放回调归还 malloc），flush 无排空语义。
 * 只暴露线性布局；压缩/瓦片化输出需要显式 DRM modifier 契约，刻意不做。
 */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "im2d.h"

/** RGA 设备节点候选（RK35xx 主线内核为 /dev/rga，旧平台为 /dev/accel0）。 */
static const char *const RGA_DEVICE_NODES[] = {"/dev/rga", "/dev/accel0"};

/** UPSCALE 未指定目标几何时的默认放大倍率。 */
#define RGA_UPSCALE_DEFAULT_FACTOR 2u

/** RGA 单边尺寸上界（硬件代际保守值；超出由 imresize 状态码兜底）。 */
#define RGA_MAX_DIMENSION 8192u

/** rkvc 像素格式映射到 RGA 格式；不支持返回 -1。 */
static int rga_format_of(rkvc_frame_fmt fmt) {
    switch (fmt) {
    case RKVC_FRAME_FMT_NV12:    return RK_FORMAT_YCbCr_420_SP;
    case RKVC_FRAME_FMT_NV21:    return RK_FORMAT_YCrCb_420_SP;
    case RKVC_FRAME_FMT_YUV420P: return RK_FORMAT_YCbCr_420_P;
    case RKVC_FRAME_FMT_RGB24:   return RK_FORMAT_RGB_888;
    default:                     return -1;
    }
}

/** 按格式计算 stride×ver_stride 布局下的总字节数；不支持返回 0。 */
static uint64_t rga_plane_bytes(rkvc_frame_fmt fmt, uint32_t stride,
                                uint32_t ver_stride) {
    uint64_t y = (uint64_t)stride * ver_stride;
    switch (fmt) {
    case RKVC_FRAME_FMT_NV12:
    case RKVC_FRAME_FMT_NV21:
    case RKVC_FRAME_FMT_YUV420P:
        return y * 3 / 2; /* UV 平面合计为 Y 平面一半 */
    case RKVC_FRAME_FMT_RGB24:
        return y * 3;
    default:
        return 0;
    }
}

/* rkvc_g_free 语义的普通 free 包装（后端 DSO 直接链接 libc 分配器）。 */
static void rga_free_buffer(void *ptr) {
    free(ptr);
}

/** rga.scale 节点私有状态（逐帧无状态，仅保存请求副本）。 */
struct rga_scaler {
    rkvc_request request; /**< 请求副本（目标几何/操作类型） */
};

/**
 * 声明端口偏好：输入接受上游任意已解析格式（UNKNOWN 通配，含 DMABUF
 * 域）；输出格式与输入相同、几何到首帧才可知，故同样声明为通配。
 */
static int rga_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec any = {0};
    (void)diag;

    rkvc_port_set_desired(&node->in_ports[0], &any);
    rkvc_port_set_desired(&node->out_ports[0], &any);
    return 0;
}

/** 验证 RGA 设备与运行库可用（librga 版本串即握手结果）。 */
static int rga_open(rkvc_node *node, rkvc_diag **diag) {
    const char *version;
    size_t i;
    int found = 0;

    for (i = 0; i < sizeof(RGA_DEVICE_NODES) / sizeof(RGA_DEVICE_NODES[0]);
         ++i) {
        if (access(RGA_DEVICE_NODES[i], R_OK | W_OK) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "no accessible RGA device node");
        return (int)RKVC_STATUS_HW;
    }
    version = querystring(RGA_VERSION);
    if (!version || !version[0]) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "librga driver query returned no version");
        return (int)RKVC_STATUS_HW;
    }
    return 0;
}

/** 计算目标几何：UPSCALE 恒 2×（width/height 是输入几何，不参与目标）；
 *  其余操作显式几何优先，缺失时按源几何直通。 */
static int rga_target_geometry(const struct rga_scaler *sc, uint32_t in_w,
                               uint32_t in_h, uint32_t *out_w,
                               uint32_t *out_h) {
    uint32_t factor = RGA_UPSCALE_DEFAULT_FACTOR;

    if (!in_w || !in_h || in_w > RGA_MAX_DIMENSION ||
        in_h > RGA_MAX_DIMENSION)
        return (int)RKVC_STATUS_FORMAT;
    if (sc->request.operation == RKVC_OPERATION_UPSCALE) {
        *out_w = in_w * factor;
        *out_h = in_h * factor;
    } else if (sc->request.width && sc->request.height) {
        *out_w = sc->request.width;
        *out_h = sc->request.height;
    } else if (sc->request.width || sc->request.height) {
        return (int)RKVC_STATUS_FORMAT; /* 目标几何必须成对提供 */
    } else {
        *out_w = in_w; /* 未指定：几何直通 */
        *out_h = in_h;
    }
    if (!*out_w || !*out_h || *out_w > RGA_MAX_DIMENSION ||
        *out_h > RGA_MAX_DIMENSION)
        return (int)RKVC_STATUS_FORMAT;
    return 0;
}

/** 缩放一帧：包装输入（HOST/DMABUF）与输出缓冲，imresize 后发出。 */
static int rga_process(rkvc_node *node, rkvc_frame *input, rkvc_diag **diag) {
    struct rga_scaler *sc = node->priv;
    rkvc_frame_desc desc;
    rkvc_frame_desc out_desc;
    rkvc_frame *output = NULL;
    rga_buffer_t src, dst;
    unsigned char *buffer = NULL;
    uint64_t buffer_size;
    uint32_t out_w, out_h, src_stride, src_vstride;
    int rga_fmt;
    IM_STATUS ret;
    rkvc_status st;
    int rc;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;
    rga_fmt = rga_format_of(desc.spec.fmt);
    if (rga_fmt < 0 || !desc.spec.width || !desc.spec.height)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF && desc.fd < 0)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.domain == RKVC_MEM_DOMAIN_HOST && !desc.data)
        return (int)RKVC_STATUS_FORMAT;
    rc = rga_target_geometry(sc, desc.spec.width, desc.spec.height,
                             &out_w, &out_h);
    if (rc != 0)
        return rc;

    /* 输出采用紧凑线性布局：stride=宽度、ver_stride=高度。 */
    buffer_size = rga_plane_bytes(desc.spec.fmt, out_w, out_h);
    if (!buffer_size)
        return (int)RKVC_STATUS_FORMAT;
    buffer = malloc(buffer_size);
    if (!buffer)
        return (int)RKVC_STATUS_NOMEM;

    src_stride = desc.spec.stride ? desc.spec.stride : desc.spec.width;
    src_vstride = desc.spec.ver_stride ? desc.spec.ver_stride
                                       : desc.spec.height;
    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF)
        src = wrapbuffer_fd_t(desc.fd, (int)desc.spec.width,
                              (int)desc.spec.height, (int)src_stride,
                              (int)src_vstride, rga_fmt);
    else
        src = wrapbuffer_virtualaddr_t(desc.data, (int)desc.spec.width,
                                       (int)desc.spec.height,
                                       (int)src_stride, (int)src_vstride,
                                       rga_fmt);
    dst = wrapbuffer_virtualaddr_t(buffer, (int)out_w, (int)out_h,
                                   (int)out_w, (int)out_h, rga_fmt);

    /* 上采样统一 bicubic（与 SR 模型训练基座一致的插值族）。
     * librga 成功返回 SUCCESS(1)（同步任务）或 NOERROR(2)（异步受理）。 */
    ret = imresize_t(src, dst, 0, 0, IM_INTERP_CUBIC, 1);
    if (ret != IM_STATUS_SUCCESS && ret != IM_STATUS_NOERROR) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           imStrError_t(ret));
        free(buffer);
        return (int)RKVC_STATUS_HW;
    }

    rkvc_frame_desc_init(&out_desc, sizeof(out_desc));
    out_desc.spec.width = out_w;
    out_desc.spec.height = out_h;
    out_desc.spec.fmt = desc.spec.fmt;
    out_desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    out_desc.spec.stride = out_w;
    out_desc.spec.ver_stride = out_h;
    out_desc.data = buffer;
    out_desc.size = (size_t)buffer_size;
    out_desc.pts = desc.pts;
    out_desc.dts = desc.dts;
    out_desc.flags = desc.flags;
    st = rkvc_backend_frame_create(&out_desc, rga_free_buffer, buffer,
                                   &output);
    if (st != RKVC_STATUS_OK) {
        free(buffer);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    return rc;
}

/* ── 节点生命周期 / 工厂 / 注册（对齐 backend_mpp.c 的结构） ──────── */

/** 通用节点析构：无 open 资源，仅释放 priv 与端口数组。 */
static void rga_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops rga_scale_ops = {
    "rga.scale", rga_configure, rga_open, rga_process, NULL, NULL,
    rga_destroy_node,
};

/** 工厂门控：UPSCALE（无模型时的硬件回退）与 TRANSCODE 显式改尺寸。 */
static int rga_scale_matches(rkvc_operation op, rkvc_codec codec,
                             const rkvc_device_caps *caps) {
    (void)codec;
    (void)caps;
    return op == RKVC_OPERATION_UPSCALE || op == RKVC_OPERATION_TRANSCODE;
}

/** 加分项：realtime 策略下优先硬件搬运路径。 */
static int rga_scale_score(const rkvc_request *request,
                           const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_REALTIME ? 60 : 40;
}

/** 创建 rga.scale 节点（单入单出 "video" 端口）。 */
static rkvc_node *rga_scale_create(const rkvc_node_factory *factory,
                                   const rkvc_request *request,
                                   void *create_ctx) {
    rkvc_node *node = calloc(1, sizeof(*node));
    struct rga_scaler *sc = calloc(1, sizeof(*sc));
    (void)factory;
    (void)create_ctx;
    if (!node || !sc) {
        free(node);
        free(sc);
        return NULL;
    }
    sc->request = *request;
    node->ops = &rga_scale_ops;
    node->priv = sc;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        rga_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "video";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = "video";
    return node;
}

/** 设备探测：设备节点可访问且 librga 握手成功。 */
static int rga_probe(const rkvc_device_caps *caps, void *probe_ctx,
                     rkvc_diag **diag) {
    size_t i;
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    for (i = 0; i < sizeof(RGA_DEVICE_NODES) / sizeof(RGA_DEVICE_NODES[0]);
         ++i) {
        if (access(RGA_DEVICE_NODES[i], R_OK | W_OK) == 0) {
            const char *version = querystring(RGA_VERSION);
            return (version && version[0]) ? 0 : -ENOTSUP;
        }
    }
    return -errno;
}

/** 工厂表：仅 rga.scale 一项。 */
static const rkvc_node_factory rga_factories[] = {
    {
        .id = "rga.scale",
        .backend_id = "rga",
        .stage = RKVC_NODE_STAGE_TRANSFORM,
        .priority = 500,
        .matches = rga_scale_matches,
        .score = rga_scale_score,
        .create = rga_scale_create,
    },
};

/** factories 回调：返回静态表。 */
static const rkvc_node_factory *rga_factory_list(void *probe_ctx,
                                                 size_t *count) {
    (void)probe_ctx;
    *count = sizeof(rga_factories) / sizeof(rga_factories[0]);
    return rga_factories;
}

/** 经 rkvc_backend_query() 导出的后端描述符。 */
static const rkvc_backend rga_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "rga",
    .capability_flags = RKVC_BACKEND_CAP_RGA,
    .probe = rga_probe,
    .factories = rga_factory_list,
};

/** DSO 入口：返回静态后端描述符。 */
const rkvc_backend *rkvc_backend_query(void) { return &rga_backend; }
