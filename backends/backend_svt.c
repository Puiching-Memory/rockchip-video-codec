/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_svt.c
 * @brief 面向版本化 rkvc ABI 的 SVT-AV1 软件编码后端（纯 CPU，无设备依赖）。
 *
 * svt.encode：ENCODE/TRANSCODE 且目标编码为 AV1 时的软件编码路径。
 * 接受上游 NV12/YUV420P（HOST 或 DMA-BUF，DMA-BUF 经 mmap 拷贝）帧，
 * 内部转换为 I420 后送入 SVT-AV1，输出带 AV1 OBU 的 HOST BITSTREAM
 * 帧（首帧含序列头，供下游 muxer 生成 av1C）。
 *
 * 与 mpp.encode 相同：几何未知时延迟到首帧初始化；编码器输出帧不写
 * 宽高（BITSTREAM 帧的宽高由请求/解析器决定）。
 */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __linux__
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "EbSvtAv1Enc.h"

#define SVT_DEFAULT_FPS_NUM 30u
#define SVT_DEFAULT_FPS_DEN 1u
#define SVT_DEFAULT_GOP     60u

/** 统一 90 kHz 时间戳基准（与 ffmpeg.backend 一致）；无输入时间戳时
 * 按 30 fps 生成 3000 刻/帧。 */
#define SVT_TS_BASE 90000
#define SVT_TS_TICK (SVT_TS_BASE / SVT_DEFAULT_FPS_NUM)

/** 编码节点私有状态。 */
struct svt_encoder {
    rkvc_request    request;   /**< 请求副本（编码/质量） */
    EbComponentType *handle;   /**< SVT 编码器句柄；未初始化前为 NULL */
    EbSvtAv1EncConfiguration config; /**< 编码配置（init_handle 填充默认值） */
    EbBufferHeaderType in_hdr; /**< 持久输入头（栈对象会随 send 返回失效） */
    EbSvtIOFormat     in_io;   /**< 持久 I/O 平面描述 */
    uint32_t        width;     /**< 首帧协商出的宽度 */
    uint32_t        height;    /**< 首帧协商出的高度 */
    uint8_t        *i420;      /**< NV12 → I420 转换缓冲 */
    size_t          i420_cap;  /**< 转换缓冲容量 */
    uint64_t        frame_index; /**< 输入帧计数（时间戳回退） */
    int             initialized; /**< 已完成 svt_av1_enc_init */
    int             eos_sent;    /**< 已向编码器发送 EOS 输入头 */
};

/** 帧释放回调：HOST 缓冲由本后端分配，直接归还。 */
static void svt_frame_release(void *ptr) {
    free(ptr);
}

#ifdef __linux__
/** DMA-BUF 读同步 ioctl（best effort）。 */
static void svt_dmabuf_read_sync(int fd, unsigned long flags) {
    struct dma_buf_sync sync = {flags};
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}
#endif

/** 把一个已就绪的编码包复制为 BITSTREAM 帧并发出。 */
static int svt_emit_packet(rkvc_node *node, EbBufferHeaderType *hdr,
                           rkvc_diag **diag) {
    struct svt_encoder *enc = node->priv;
    rkvc_frame_desc desc;
    rkvc_frame *frame = NULL;
    rkvc_status st;
    void *data;
    int rc;

    if (!hdr || !hdr->p_buffer || !hdr->n_filled_len)
        return 0;
    data = malloc(hdr->n_filled_len);
    if (!data)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(data, hdr->p_buffer, hdr->n_filled_len);

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.data = data;
    desc.size = hdr->n_filled_len;
    desc.pts = hdr->pts;
    desc.dts = hdr->dts;
    if (hdr->pic_type == EB_AV1_KEY_PICTURE)
        desc.flags |= RKVC_FRAME_FLAG_KEYFRAME;
    st = rkvc_backend_frame_create(&desc, svt_frame_release, data, &frame);
    if (st != RKVC_STATUS_OK) {
        free(data);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, frame);
    if (rc != 0)
        rkvc_frame_release(frame);
    (void)enc;
    (void)diag;
    return rc;
}

/** 反复取出已就绪包并发出；SVT 无包时返回 EB_NoErrorEmptyQueue。 */
static int svt_drain_packets(rkvc_node *node, rkvc_diag **diag) {
    struct svt_encoder *enc = node->priv;

    for (;;) {
        EbBufferHeaderType *hdr = NULL;
        /* EOS 已送后必须传 pic_send_done=1：RANDOM_ACCESS 下传 0 是
         * 非阻塞轮询，队列恰好为空时会误判"编完了"而丢弃在途帧。
         * 传 1 则阻塞取包直到拿到带 EOS 标记的包，其后再调用才返回
         * EB_NoErrorEmptyQueue。EOS 未发送前禁止传 1（库内断言）。 */
        uint8_t pic_send_done = (uint8_t)(enc->eos_sent != 0);
        EbErrorType ret;
        int rc;

        ret = svt_av1_enc_get_packet(enc->handle, &hdr, pic_send_done);
        if (ret == EB_NoErrorEmptyQueue)
            return 0;
        if (ret != EB_ErrorNone) {
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                               "svt encoder get_packet failed");
            return (int)RKVC_STATUS_HW;
        }
        if (!hdr)
            continue;
        rc = svt_emit_packet(node, hdr, diag);
        svt_av1_enc_release_out_buffer(&hdr);
        if (rc != 0)
            return rc;
    }
}

/** 初始化 SVT 编码器（几何已知后调用一次）。 */
static int svt_init_locked(struct svt_encoder *enc, rkvc_diag **diag) {
    EbSvtAv1EncConfiguration *cfg = &enc->config;
    EbErrorType ret;

    memset(cfg, 0, sizeof(*cfg));
    ret = svt_av1_enc_init_handle(&enc->handle, cfg);
    if (ret != EB_ErrorNone)
        goto fail;

    cfg->source_width  = enc->width;
    cfg->source_height = enc->height;
    cfg->encoder_bit_depth = 8;
    cfg->encoder_color_format = EB_YUV420;
    cfg->frame_rate_numerator = SVT_DEFAULT_FPS_NUM;
    cfg->frame_rate_denominator = SVT_DEFAULT_FPS_DEN;
    cfg->intra_period_length = SVT_DEFAULT_GOP;
    cfg->pred_structure = RANDOM_ACCESS;
    if (enc->request.quality.qp >= 0) {
        /* 固定 QP / CRF：直接使用请求 QP。 */
        cfg->rate_control_mode = SVT_AV1_RC_MODE_CQP_OR_CRF;
        cfg->qp = (uint32_t)enc->request.quality.qp;
    } else {
        int64_t bps = enc->request.quality.bitrate_bps > 0
                          ? enc->request.quality.bitrate_bps
                          : (int64_t)enc->width * enc->height * 3;
        /* EbSvtAv1EncConfiguration::target_bit_rate 单位是 bits/s
         * （头注释与日志打印 /1000 转 kbps 均一致）。
         * 4.x 校验：max_bit_rate != 0 仅允许 CQP_OR_CRF 模式，
         * VBR 只设 target_bit_rate。 */
        cfg->rate_control_mode = SVT_AV1_RC_MODE_VBR;
        cfg->target_bit_rate = (uint32_t)bps;
    }
    ret = svt_av1_enc_set_parameter(enc->handle, cfg);
    if (ret != EB_ErrorNone)
        goto fail;
    ret = svt_av1_enc_init(enc->handle);
    if (ret != EB_ErrorNone)
        goto fail;
    enc->initialized = 1;
    return 0;

fail:
    if (diag)
        rkvc_diag_push(diag, RKVC_STATUS_HW, 3, "svt.encode",
                       "svt encoder init failed");
    if (enc->handle) {
        svt_av1_enc_deinit(enc->handle);
        svt_av1_enc_deinit_handle(enc->handle);
        enc->handle = NULL;
    }
    enc->initialized = 0;
    return (int)RKVC_STATUS_HW;
}

/** 把 NV12/YUV420P 帧（HOST 或 DMA-BUF）转换为 I420 写入 enc->i420。 */
static int svt_convert_input(struct svt_encoder *enc,
                             const rkvc_frame_desc *desc,
                             const uint8_t *base, rkvc_diag **diag) {
    uint32_t w = enc->width, h = enc->height;
    uint32_t cw = w / 2, ch = h / 2;
    uint32_t ystride = desc->spec.stride ? desc->spec.stride : w;
    uint32_t vstride = desc->spec.ver_stride ? desc->spec.ver_stride : h;
    uint8_t *dst;
    const uint8_t *y = base;
    const uint8_t *uv = base + (size_t)ystride * vstride;
    uint32_t row;
    int rc = 0;

    if (desc->spec.fmt != RKVC_FRAME_FMT_NV12 &&
        desc->spec.fmt != RKVC_FRAME_FMT_YUV420P) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, "svt.encode",
                           "svt accepts NV12/YUV420P input only");
        return (int)RKVC_STATUS_FORMAT;
    }
    if (enc->i420_cap < (size_t)w * h * 3 / 2) {
        /* 尾部留余量：防御 NEON 内核对帧末的宽读。 */
        size_t alloc = (size_t)w * h * 3 / 2 + 64 * 1024;
        uint8_t *nb = realloc(enc->i420, alloc);
        if (!nb)
            return (int)RKVC_STATUS_NOMEM;
        enc->i420 = nb;
        enc->i420_cap = (size_t)w * h * 3 / 2;
    }
    /* 必须在 realloc 之后取指针：缓冲可能刚分配或被移动。 */
    dst = enc->i420;

    /* Y 平面逐行拷贝（裁剪 stride 填充）。 */
    for (row = 0; row < h; ++row)
        memcpy(dst + (size_t)row * w, y + (size_t)row * ystride, w);

    if (desc->spec.fmt == RKVC_FRAME_FMT_NV12) {
        /* UV 交错 → 分离 U/V 平面。 */
        for (row = 0; row < ch; ++row) {
            const uint8_t *s = uv + (size_t)row * ystride;
            uint8_t *u = dst + (size_t)w * h + (size_t)row * cw;
            uint8_t *v = u + (size_t)cw * ch;
            uint32_t x;
            for (x = 0; x < cw; ++x) {
                u[x] = s[2 * x];
                v[x] = s[2 * x + 1];
            }
        }
    } else {
        /* YUV420P：三平面拷贝。 */
        const uint8_t *u = base + (size_t)ystride * vstride;
        const uint8_t *v = u + (size_t)ystride * vstride / 4;
        uint32_t us = ystride / 2;
        uint8_t *du = dst + (size_t)w * h;
        uint8_t *dv = du + (size_t)cw * ch;
        for (row = 0; row < ch; ++row) {
            memcpy(du + (size_t)row * cw, u + (size_t)row * us, cw);
            memcpy(dv + (size_t)row * cw, v + (size_t)row * us, cw);
        }
    }
    return rc;
}

/** 编码一帧：首帧初始化、转换输入、送入 SVT 并排空已就绪包。 */
static int svt_process(rkvc_node *node, rkvc_frame *input,
                       rkvc_diag **diag) {
    struct svt_encoder *enc = node->priv;
    rkvc_frame_desc desc;
    const uint8_t *base;
    void *mapped = NULL;
    EbBufferHeaderType *hdr = &enc->in_hdr;
    EbSvtIOFormat *io = &enc->in_io;
    EbErrorType ret;
    int rc;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF) {
#ifdef __linux__
        if (desc.size == 0)
            return (int)RKVC_STATUS_FORMAT;
        mapped = mmap(NULL, desc.size, PROT_READ, MAP_SHARED, desc.fd, 0);
        if (mapped == MAP_FAILED)
            return (int)RKVC_STATUS_IO;
        svt_dmabuf_read_sync(desc.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        base = mapped;
#else
        return (int)RKVC_STATUS_UNSUPPORTED;
#endif
    } else {
        if (!desc.data && desc.size)
            return (int)RKVC_STATUS_FORMAT;
        base = desc.data;
    }

    if (!enc->initialized) {
        if (!desc.spec.width || !desc.spec.height ||
            (desc.spec.width & 1u) || (desc.spec.height & 1u)) {
            if (mapped) {
#ifdef __linux__
                svt_dmabuf_read_sync(desc.fd,
                                     DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                munmap(mapped, desc.size);
#endif
            }
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3,
                               node->ops->id,
                               "svt requires even width/height");
            return (int)RKVC_STATUS_FORMAT;
        }
        enc->width = desc.spec.width;
        enc->height = desc.spec.height;
        rc = svt_init_locked(enc, diag);
        if (rc != 0) {
            if (mapped) {
#ifdef __linux__
                svt_dmabuf_read_sync(desc.fd,
                                     DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
                munmap(mapped, desc.size);
#endif
            }
            return rc;
        }
    } else if (desc.spec.width != enc->width ||
               desc.spec.height != enc->height) {
        if (mapped) {
#ifdef __linux__
            svt_dmabuf_read_sync(desc.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            munmap(mapped, desc.size);
#endif
        }
        return (int)RKVC_STATUS_FORMAT;
    }

    rc = svt_convert_input(enc, &desc, base, diag);
    if (mapped) {
#ifdef __linux__
        svt_dmabuf_read_sync(desc.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        munmap(mapped, desc.size);
#endif
    }
    if (rc != 0)
        return rc;

    memset(io, 0, sizeof(*io));
    io->luma = enc->i420;
    io->cb = enc->i420 + (size_t)enc->width * enc->height;
    io->cr = io->cb + (size_t)(enc->width / 2) * (enc->height / 2);
    io->y_stride = enc->width;
    io->cb_stride = enc->width / 2;
    io->cr_stride = enc->width / 2;

    memset(hdr, 0, sizeof(*hdr));
    hdr->size = (uint32_t)sizeof(*hdr);
    hdr->p_buffer = (uint8_t *)io;
    hdr->n_filled_len =
        (uint32_t)((size_t)enc->width * enc->height * 3 / 2);
    hdr->pic_type = EB_AV1_INVALID_PICTURE;
    if (desc.encode.force_idr)
        hdr->pic_type = EB_AV1_KEY_PICTURE;
    if (desc.pts != RKVC_FRAME_TS_UNKNOWN && desc.pts >= 0) {
        hdr->pts = desc.pts;
        hdr->dts = desc.dts >= 0 ? desc.dts : desc.pts;
    } else {
        hdr->pts = (int64_t)enc->frame_index * SVT_TS_TICK;
        hdr->dts = hdr->pts;
    }
    ret = svt_av1_enc_send_picture(enc->handle, hdr);
    if (ret != EB_ErrorNone) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "svt send_picture failed");
        return (int)RKVC_STATUS_HW;
    }
    enc->frame_index++;
    rc = svt_drain_packets(node, diag);
    return rc;
}

/** 提交 EOS 并排空全部剩余编码包。 */
static int svt_flush(rkvc_node *node, rkvc_diag **diag) {
    struct svt_encoder *enc = node->priv;
    EbErrorType ret;
    int rc;

    if (!enc->initialized)
        return 0; /* 空输入流：编码器从未初始化 */
    memset(&enc->in_hdr, 0, sizeof(enc->in_hdr));
    enc->in_hdr.size = (uint32_t)sizeof(enc->in_hdr);
    enc->in_hdr.flags = EB_BUFFERFLAG_EOS;
    enc->in_hdr.pic_type = EB_AV1_INVALID_PICTURE;
    ret = svt_av1_enc_send_picture(enc->handle, &enc->in_hdr);
    if (ret != EB_ErrorNone) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "svt eos send failed");
        return (int)RKVC_STATUS_HW;
    }
    enc->eos_sent = 1;
    rc = svt_drain_packets(node, diag);
    return rc;
}

/** 释放 SVT 句柄与转换缓冲（幂等）。 */
static void svt_close(rkvc_node *node) {
    struct svt_encoder *enc = node->priv;
    if (!enc)
        return;
    if (enc->handle) {
        svt_av1_enc_deinit(enc->handle);
        svt_av1_enc_deinit_handle(enc->handle);
        enc->handle = NULL;
    }
    enc->initialized = 0;
    free(enc->i420);
    enc->i420 = NULL;
    enc->i420_cap = 0;
}

/* ═══════════════════ 节点生命周期 / 工厂 / 注册 ═══════════════════ */

/** 通用节点析构：close 后释放 priv 与端口数组。 */
static void svt_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

/** 接受上游任意已解析格式；声明 BITSTREAM/HOST 输出。 */
static int svt_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec input = {0};   /* UNKNOWN：接受上游任意已解析格式 */
    rkvc_frame_spec output = {0};
    (void)diag;

    output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    output.domain = RKVC_MEM_DOMAIN_HOST;
    rkvc_port_set_desired(&node->in_ports[0], &input);
    rkvc_port_set_desired(&node->out_ports[0], &output);
    return 0;
}

/** open 无硬资源：几何未知时延迟到首帧（与 mpp.encode 一致）。 */
static int svt_open(rkvc_node *node, rkvc_diag **diag) {
    (void)node;
    (void)diag;
    return 0;
}

static const rkvc_node_ops svt_enc_ops = {
    "svt.encode", svt_configure, svt_open, svt_process,
    svt_flush, svt_close, svt_destroy_node,
};

/** 编码工厂门控：ENCODE/TRANSCODE 且目标为 AV1。 */
static int svt_enc_matches(rkvc_operation op, rkvc_codec codec,
                           const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_ENCODE || op == RKVC_OPERATION_TRANSCODE) &&
           codec == RKVC_CODEC_AV1;
}

/** 加分项：offline/quality 策略下优先软件高质量编码。 */
static int svt_score(const rkvc_request *request,
                     const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_QUALITY ||
                   request->policy == RKVC_POLICY_OFFLINE
               ? 50
               : 0;
}

/** create：单输入 "video"、单输出 "bitstream"。 */
static rkvc_node *svt_node_create(const rkvc_node_factory *factory,
                                  const rkvc_request *request,
                                  void *create_ctx) {
    rkvc_node *node;
    struct svt_encoder *enc;
    (void)factory;
    (void)create_ctx;

    node = calloc(1, sizeof(*node));
    enc = calloc(1, sizeof(*enc));
    if (!node || !enc) {
        free(node);
        free(enc);
        return NULL;
    }
    enc->request = *request;
    node->ops = &svt_enc_ops;
    node->priv = enc;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        svt_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "video";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = "bitstream";
    return node;
}

/** probe：纯软件后端，恒通过。 */
static int svt_probe(const rkvc_device_caps *caps, void *probe_ctx,
                     rkvc_diag **diag) {
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    return 0;
}

/** 工厂表：单条编码工厂。 */
static const rkvc_node_factory svt_factories[] = {
    {
        .id = "svt.encode",
        .backend_id = "svt",
        .stage = RKVC_NODE_STAGE_ENCODE,
        .priority = 500,
        .matches = svt_enc_matches,
        .score = svt_score,
        .create = svt_node_create,
    },
};

static const rkvc_node_factory *svt_factory_list(void *probe_ctx,
                                                 size_t *count) {
    (void)probe_ctx;
    *count = sizeof(svt_factories) / sizeof(svt_factories[0]);
    return svt_factories;
}

/** 经 rkvc_backend_query() 导出的后端描述符。 */
static const rkvc_backend svt_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "svt",
    .capability_flags = 0,
    .probe = svt_probe,
    .factories = svt_factory_list,
};

/** DSO 入口：返回静态后端描述符。 */
const rkvc_backend *rkvc_backend_query(void) { return &svt_backend; }
