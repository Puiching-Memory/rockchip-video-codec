/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file node_fileio.c
 * @brief 内建 fileio 后端：文件 source/sink 节点。
 *
 * - file.source：FILE 输入端点。DECODE/TRANSCODE 按块产出 BITSTREAM 帧
 *   （解码器侧 split mode 负责切帧）；ENCODE 与 UPSCALE（提供 width/
 *   height 输入几何时）按 NV12 帧尺寸产出原始帧。源节点无输入端口，
 *   文件消费发生在 flush（start 时输入队列自动 EOS）。
 * - file.sink：FILE 输出端点。BITSTREAM 帧按载荷写出；NV12/P010/YUV420P
 *   原始帧按可见宽度逐行写出（裁剪 stride 填充）。DMA-BUF 帧经 mmap 读取。
 *
 * 该后端无外部依赖，始终可注册；单元测试借此在 x86 上端到端跑通
 * 规划器 source/sink 注入与执行器背压/EOS 路径。
 */

#include "context_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define FILEIO_BITSTREAM_CHUNK (256u * 1024u)

/* rkvc_g_free 在单测构建下是函数式宏，不能直接当函数指针使用。 */
static void fileio_frame_release(void *ptr) {
    rkvc_g_free(ptr);
}

/* ── file.source ─────────────────────────────────────────────────── */

/** file.source 节点私有状态。 */
typedef struct file_source {
    rkvc_request request; /**< 请求副本（读取 uri/操作类型） */
    FILE        *fp;      /**< flush 阶段打开的输入文件 */
} file_source;

/** 声明输出格式：DECODE/TRANSCODE 为 BITSTREAM；ENCODE 为 NV12；
 * UPSCALE 几何已知时按 NV12 原始帧产出（transform 直接消费）。 */
static int source_configure(rkvc_node *node, rkvc_diag **diag) {
    file_source *src = node->priv;
    rkvc_frame_spec out = {0};

    switch (src->request.operation) {
    case RKVC_OPERATION_DECODE:
    case RKVC_OPERATION_TRANSCODE:
        out.fmt = RKVC_FRAME_FMT_BITSTREAM;
        out.domain = RKVC_MEM_DOMAIN_HOST;
        break;
    case RKVC_OPERATION_UPSCALE:
        if (!src->request.width || !src->request.height) {
            /* 几何未知：整文件按 BITSTREAM 产出。当前没有图像 demux，
             * transform 节点会在协商/处理期拒绝并留下诊断。 */
            out.fmt = RKVC_FRAME_FMT_BITSTREAM;
            out.domain = RKVC_MEM_DOMAIN_HOST;
            break;
        }
        /* 几何已知：与 ENCODE 同路径，输入按 NV12 原始帧解释。 */
        out.fmt = RKVC_FRAME_FMT_NV12;
        out.domain = RKVC_MEM_DOMAIN_HOST;
        out.width = src->request.width;
        out.height = src->request.height;
        out.stride = src->request.width;
        break;
    case RKVC_OPERATION_ENCODE:
        if (!src->request.width || !src->request.height) {
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 1, node->ops->id,
                               "encode source requires width/height");
            return (int)RKVC_STATUS_FORMAT;
        }
        out.fmt = RKVC_FRAME_FMT_NV12;
        out.domain = RKVC_MEM_DOMAIN_HOST;
        out.width = src->request.width;
        out.height = src->request.height;
        out.stride = src->request.width;
        break;
    default:
        return (int)RKVC_STATUS_INVALID;
    }
    rkvc_port_set_desired(&node->out_ports[0], &out);
    return 0;
}

/** 打开输入文件（实例化阶段；失败产生 IO 诊断）。 */
static int source_open(rkvc_node *node, rkvc_diag **diag) {
    file_source *src = node->priv;

    src->fp = fopen(src->request.input.uri, "rb");
    if (!src->fp) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_IO, 3, node->ops->id,
                           "cannot open input file");
        return (int)RKVC_STATUS_IO;
    }
    return 0;
}

/** 把一块读入的数据包装为帧并推入输出端口；失败时自行释放缓冲。 */
static int source_emit(rkvc_node *node, const rkvc_frame_spec *spec,
                       void *data, size_t size) {
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    rkvc_status st;
    int rc;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec = *spec;
    desc.data = data;
    desc.size = size;
    st = rkvc_backend_frame_create(&desc, fileio_frame_release, data, &frame);
    if (st != RKVC_STATUS_OK) {
        rkvc_g_free(data);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, frame);
    if (rc != 0)
        rkvc_frame_release(frame);
    return rc;
}

/** 源节点在 flush 阶段（输入队列 EOS 触发）产出整个文件。 */
static int source_flush(rkvc_node *node, rkvc_diag **diag) {
    file_source *src = node->priv;
    const rkvc_frame_spec *spec = &node->out_ports[0].fmt;
    size_t chunk = FILEIO_BITSTREAM_CHUNK;
    int rc = 0;

    if (!src->fp)
        return (int)RKVC_STATUS_INVALID;
    if (spec->fmt == RKVC_FRAME_FMT_NV12) {
        size_t row = spec->stride ? spec->stride : spec->width;
        chunk = row * spec->height * 3 / 2;
    }

    for (;;) {
        void *data = rkvc_g_calloc(1, chunk);
        size_t got;
        if (!data)
            return (int)RKVC_STATUS_NOMEM;
        got = fread(data, 1, chunk, src->fp);
        if (got == 0) {
            rkvc_g_free(data);
            if (ferror(src->fp)) {
                if (diag)
                    rkvc_diag_push(diag, RKVC_STATUS_IO, 3, node->ops->id,
                                   "input read failed");
                return (int)RKVC_STATUS_IO;
            }
            break; /* EOF */
        }
        if (spec->fmt == RKVC_FRAME_FMT_NV12 && got != chunk) {
            rkvc_g_free(data);
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                               "trailing partial raw frame");
            return (int)RKVC_STATUS_FORMAT;
        }
        rc = source_emit(node, spec, data, got);
        if (rc != 0)
            return rc; /* 取消/错误时所有权已归还，缓冲已释放 */
    }
    return 0;
}

/** 关闭输入文件（幂等）。 */
static void source_close(rkvc_node *node) {
    file_source *src = node->priv;
    if (src && src->fp) {
        fclose(src->fp);
        src->fp = NULL;
    }
}

/** 通用销毁：先 close 再释放节点与端口数组（source/sink 共用）。 */
static void fileio_destroy(rkvc_node *node) {
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    rkvc_g_free(node->priv);
    rkvc_g_free(node->in_ports);
    rkvc_g_free(node->out_ports);
    rkvc_g_free(node);
}

static const rkvc_node_ops source_ops = {
    "file.source", source_configure, source_open, NULL, source_flush,
    source_close, fileio_destroy,
};

/** matches 回调：纯软件节点对任意操作/编码/设备恒可用。 */
static int fileio_matches_any(rkvc_operation op, rkvc_codec codec,
                              const rkvc_device_caps *caps) {
    (void)op;
    (void)codec;
    (void)caps;
    return 1;
}

/** 创建 file.source 节点（单输出端口 "out"；仅接受 FILE 输入端点）。 */
static rkvc_node *source_create(const rkvc_node_factory *factory,
                                const rkvc_request *request,
                                void *create_ctx) {
    rkvc_node *node;
    file_source *src;
    (void)factory;
    (void)create_ctx;

    if (!request->input.uri || request->input.kind != RKVC_ENDPOINT_FILE)
        return NULL;
    node = rkvc_g_calloc(1, sizeof(*node));
    src = rkvc_g_calloc(1, sizeof(*src));
    if (!node || !src) {
        rkvc_g_free(node);
        rkvc_g_free(src);
        return NULL;
    }
    src->request = *request;
    node->ops = &source_ops;
    node->priv = src;
    node->out_ports = rkvc_g_calloc(1, sizeof(*node->out_ports));
    if (!node->out_ports) {
        fileio_destroy(node);
        return NULL;
    }
    node->out_count = 1;
    node->out_ports[0].name = "out";
    return node;
}

/* ── file.sink ───────────────────────────────────────────────────── */

/** file.sink 节点私有状态与写出统计。 */
typedef struct file_sink {
    rkvc_request request; /**< 请求副本（读取 output.uri） */
    FILE        *fp;      /**< 打开的输出文件 */
    uint64_t     frames;  /**< 已写出帧数 */
    uint64_t     bytes;   /**< 已写出字节数 */
} file_sink;

/** 接受上游任意已解析格式（UNKNOWN 通配）。 */
static int sink_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec in = {0}; /* UNKNOWN：接受上游任意已解析格式 */
    (void)diag;

    rkvc_port_set_desired(&node->in_ports[0], &in);
    return 0;
}

/** 打开输出文件（"wb"，截断已存在文件）。 */
static int sink_open(rkvc_node *node, rkvc_diag **diag) {
    file_sink *sink = node->priv;

    sink->fp = fopen(sink->request.output.uri, "wb");
    if (!sink->fp) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_IO, 3, node->ops->id,
                           "cannot open output file");
        return (int)RKVC_STATUS_IO;
    }
    return 0;
}

/** 原子写出一块数据；短写返回 IO 错误。 */
static int sink_write(file_sink *sink, const void *data, size_t size) {
    size_t put = fwrite(data, 1, size, sink->fp);
    sink->bytes += put;
    return put == size ? 0 : (int)RKVC_STATUS_IO;
}

/** 按可见宽度逐行写出 Y/UV 平面，裁剪 stride 填充。 */
static int sink_write_video(file_sink *sink, const rkvc_frame_desc *desc,
                            const unsigned char *base) {
    const rkvc_frame_spec *spec = &desc->spec;
    size_t bpp = spec->fmt == RKVC_FRAME_FMT_P010 ? 2 : 1;
    size_t visible = (size_t)spec->width * bpp;
    size_t row = spec->stride ? spec->stride : visible;
    size_t vstride = spec->ver_stride ? spec->ver_stride : spec->height;
    uint32_t y;
    int rc;

    if (!spec->width || !spec->height)
        return (int)RKVC_STATUS_FORMAT;
    if ((size_t)spec->height > SIZE_MAX / row)
        return (int)RKVC_STATUS_FORMAT;

    for (y = 0; y < spec->height; ++y) {
        rc = sink_write(sink, base + (size_t)y * row, visible);
        if (rc != 0)
            return rc;
    }
    if (spec->fmt == RKVC_FRAME_FMT_NV12 ||
        spec->fmt == RKVC_FRAME_FMT_NV21 ||
        spec->fmt == RKVC_FRAME_FMT_P010) {
        const unsigned char *uv = base + row * vstride;
        for (y = 0; y < spec->height / 2; ++y) {
            rc = sink_write(sink, uv + (size_t)y * row, visible);
            if (rc != 0)
                return rc;
        }
    } else if (spec->fmt == RKVC_FRAME_FMT_YUV420P) {
        const unsigned char *u = base + row * vstride;
        const unsigned char *v = u + row * vstride / 4;
        for (y = 0; y < spec->height / 2; ++y) {
            rc = sink_write(sink, u + (size_t)y * row / 2, visible / 2);
            if (rc != 0)
                return rc;
        }
        for (y = 0; y < spec->height / 2; ++y) {
            rc = sink_write(sink, v + (size_t)y * row / 2, visible / 2);
            if (rc != 0)
                return rc;
        }
    } else {
        return (int)RKVC_STATUS_FORMAT;
    }
    return 0;
}

#ifdef __linux__
/** DMA-BUF 读同步 ioctl（best effort，失败不影响数据读取）。 */
static void dmabuf_read_sync(int fd, unsigned long flags) {
    struct dma_buf_sync sync = {flags};
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync); /* best effort */
}
#endif

/** 处理一帧：码流按载荷直写；原始帧按平面裁剪写出（DMA-BUF 先 mmap）。 */
static int sink_process(rkvc_node *node, rkvc_frame *input,
                        rkvc_diag **diag) {
    file_sink *sink = node->priv;
    rkvc_frame_desc desc;
    const void *base;
    void *mapped = NULL;
    int rc;
    (void)diag;

    if (!sink->fp)
        return (int)RKVC_STATUS_INVALID;
    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;

    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF) {
#ifdef __linux__
        if (desc.size == 0)
            return (int)RKVC_STATUS_FORMAT;
        mapped = mmap(NULL, desc.size, PROT_READ, MAP_SHARED, desc.fd, 0);
        if (mapped == MAP_FAILED)
            return (int)RKVC_STATUS_IO;
        dmabuf_read_sync(desc.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        base = mapped;
#else
        return (int)RKVC_STATUS_UNSUPPORTED;
#endif
    } else {
        if (!desc.data && desc.size)
            return (int)RKVC_STATUS_FORMAT;
        base = desc.data;
    }

    if (desc.spec.fmt == RKVC_FRAME_FMT_BITSTREAM)
        rc = sink_write(sink, base, desc.size);
    else
        rc = sink_write_video(sink, &desc, base);

    if (mapped) {
#ifdef __linux__
        dmabuf_read_sync(desc.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        munmap(mapped, desc.size);
#endif
    }
    if (rc == 0)
        sink->frames++;
    return rc;
}

/** 关闭输出文件（幂等）。 */
static void sink_close(rkvc_node *node) {
    file_sink *sink = node->priv;
    if (sink && sink->fp) {
        fclose(sink->fp);
        sink->fp = NULL;
    }
}

static const rkvc_node_ops sink_ops = {
    "file.sink", sink_configure, sink_open, sink_process, NULL,
    sink_close, fileio_destroy,
};

/** 创建 file.sink 节点（单输入端口 "in"；仅接受 FILE 输出端点）。 */
static rkvc_node *sink_create(const rkvc_node_factory *factory,
                              const rkvc_request *request,
                              void *create_ctx) {
    rkvc_node *node;
    file_sink *sink;
    (void)factory;
    (void)create_ctx;

    if (!request->output.uri || request->output.kind != RKVC_ENDPOINT_FILE)
        return NULL;
    node = rkvc_g_calloc(1, sizeof(*node));
    sink = rkvc_g_calloc(1, sizeof(*sink));
    if (!node || !sink) {
        rkvc_g_free(node);
        rkvc_g_free(sink);
        return NULL;
    }
    sink->request = *request;
    node->ops = &sink_ops;
    node->priv = sink;
    node->in_ports = rkvc_g_calloc(1, sizeof(*node->in_ports));
    if (!node->in_ports) {
        fileio_destroy(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "in";
    node->in_ports[0].is_input = 1;
    return node;
}

/* ── 后端描述符与注册 ─────────────────────────────────────────────── */

/** fileio 后端工厂表：source 与 sink 两个条目。 */
static const rkvc_node_factory fileio_factories[] = {
    {
        .id = "file.source",
        .backend_id = "fileio",
        .stage = RKVC_NODE_STAGE_SOURCE,
        .priority = 0,
        .matches = fileio_matches_any,
        .create = source_create,
    },
    {
        .id = "file.sink",
        .backend_id = "fileio",
        .stage = RKVC_NODE_STAGE_SINK,
        .priority = 0,
        .matches = fileio_matches_any,
        .create = sink_create,
    },
};

/** factories 回调：返回静态工厂表。 */
static const rkvc_node_factory *fileio_factory_list(void *probe_ctx,
                                                    size_t *count) {
    (void)probe_ctx;
    *count = sizeof(fileio_factories) / sizeof(fileio_factories[0]);
    return fileio_factories;
}

/** probe 回调：纯软件节点，无设备依赖，恒通过。 */
static int fileio_probe(const rkvc_device_caps *caps, void *probe_ctx,
                        rkvc_diag **diag) {
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    return 0; /* 纯软件节点，始终可用 */
}

/** fileio 后端描述符（静态存储，随核心库常驻）。 */
static const rkvc_backend fileio_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "fileio",
    .capability_flags = 0,
    .probe = fileio_probe,
    .factories = fileio_factory_list,
};

void rkvc_fileio_backend_register(rkvc_context *ctx) {
    (void)rkvc_registry_add_backend(ctx, &fileio_backend);
}
