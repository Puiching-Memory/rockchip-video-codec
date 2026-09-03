/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_mpp.c
 * @brief 面向版本化 rkvc ABI 的自足 Rockchip MPP 后端：
 * H.264/HEVC/AV1 硬解 + H.264/HEVC 硬编。
 *
 * mpp.decode：请求显式指定 codec 时直接使用；AUTO（以及 TRANSCODE，
 * 其 codec 字段表达目标编码）通过首帧码流嗅探 Annex-B 参数集解决，
 * MPP 随后惰性初始化。mpp.encode 接受 NV12/YUV420P HOST 帧（拷入
 * MPP 缓冲）或 DMA-BUF 帧（fd 导入，失败回退拷贝），输出带 SPS/PPS
 * 的 HOST BITSTREAM 帧（每个 IDR 前重发头）。
 *
 * 压缩/瓦片化 MPP 布局需要显式 DRM modifier 契约；本后端刻意只暴露
 * 线性帧。
 */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#include <rk_mpi.h>

#define MPP_ENC_DEFAULT_FPS 30
#define MPP_ENC_DEFAULT_GOP 60

/** 把 MPP 输出格式映射到最接近的 rkvc 像素格式。 */
static rkvc_frame_fmt mpp_frame_format(MppFrameFormat format) {
    switch (format & MPP_FRAME_FMT_MASK) {
    case MPP_FMT_YUV420SP:
        return RKVC_FRAME_FMT_NV12;
    case MPP_FMT_YUV420SP_10BIT:
        return RKVC_FRAME_FMT_P010;
    default:
        return RKVC_FRAME_FMT_UNKNOWN;
    }
}

/** 把 rkvc 编码映射为 MPP coding 类型；AUTO 映射为 Unused。 */
static MppCodingType rkvc_to_mpp_coding(rkvc_codec codec) {
    switch (codec) {
    case RKVC_CODEC_H264: return MPP_VIDEO_CodingAVC;
    case RKVC_CODEC_HEVC: return MPP_VIDEO_CodingHEVC;
    case RKVC_CODEC_AV1:  return MPP_VIDEO_CodingAV1;
    default:              return MPP_VIDEO_CodingUnused;
    }
}

/** 在 Annex-B 码流中扫描参数集：H.264 SPS / HEVC VPS。
 * AV1 无起始码，必须显式指定编码。 */
static MppCodingType sniff_annexb_coding(const unsigned char *data,
                                         size_t size) {
    size_t i = 0;
    while (i + 4 < size) {
        size_t hdr;
        unsigned char nal;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
            hdr = 3;
        else if (i + 5 <= size && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1)
            hdr = 4;
        else {
            ++i;
            continue;
        }
        nal = data[i + hdr];
        if ((nal & 0x1f) == 7)               /* H.264 SPS */
            return MPP_VIDEO_CodingAVC;
        if (((nal >> 1) & 0x3f) == 32)       /* HEVC VPS */
            return MPP_VIDEO_CodingHEVC;
        i += hdr;
    }
    return MPP_VIDEO_CodingUnused;
}

/** 解码节点私有状态。 */
struct mpp_decoder {
    rkvc_request  request;     /**< 请求副本（操作/编码） */
    MppCtx        ctx;         /**< MPP 上下文；open 前为 NULL */
    MppApi       *mpi;         /**< MPP 控制 API */
    MppCodingType coding;      /* MPP_VIDEO_CodingUnused = 待首帧嗅探 */
    int           initialized; /**< 已完成 mpp_init（嗅探输入为惰性） */
    uint64_t      emitted;     /**< 已发出帧数（BUFFER_FULL 重试进展检测） */
};

/** 把一帧解码输出的 MppFrame 包装为线性 DMABUF rkvc_frame 并发出。 */
static int emit_mpp_frame(rkvc_node *node, MppFrame frame) {
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    MppFrameFormat format = mpp_frame_get_fmt(frame);
    rkvc_frame_desc desc;
    rkvc_frame *output = NULL;
    rkvc_status st;
    int rc;

    /* 压缩/瓦片化 MPP 布局需要显式 DRM modifier 契约；
     * 首个可移植后端刻意只暴露线性帧。 */
    if (!buffer || MPP_FRAME_FMT_IS_FBC(format) ||
        MPP_FRAME_FMT_IS_TILE(format))
        return (int)RKVC_STATUS_UNSUPPORTED;

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.width = mpp_frame_get_width(frame);
    desc.spec.height = mpp_frame_get_height(frame);
    desc.spec.fmt = mpp_frame_format(format);
    desc.spec.domain = RKVC_MEM_DOMAIN_DMABUF;
    desc.spec.stride = mpp_frame_get_hor_stride(frame);
    desc.spec.ver_stride = mpp_frame_get_ver_stride(frame);
    desc.size = mpp_buffer_get_size(buffer);
    desc.fd = mpp_buffer_get_fd(buffer);
    desc.pts = mpp_frame_get_pts(frame);
    desc.dts = mpp_frame_get_dts(frame);
    if (mpp_frame_get_errinfo(frame) || mpp_frame_get_discard(frame))
        desc.flags |= RKVC_FRAME_FLAG_CORRUPT;
    if (desc.spec.fmt == RKVC_FRAME_FMT_UNKNOWN || desc.fd < 0)
        return (int)RKVC_STATUS_FORMAT;

    /* 核心复制并持有 fd。因此 MppFrame 可以立即释放，
     * 输出帧也可以安全地比其后端 DSO 活得更久。 */
    st = rkvc_backend_frame_create_dmabuf(&desc, &output);
    if (st != RKVC_STATUS_OK)
        return (int)st;
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    return rc;
}

/** 反复调用 decode_get_frame 直到取干；until_eos 时必须等到 EOS 帧。 */
static int drain_decoder(rkvc_node *node, int until_eos) {
    struct mpp_decoder *dec = node->priv;

    for (;;) {
        MppFrame frame = NULL;
        MPP_RET ret = dec->mpi->decode_get_frame(dec->ctx, &frame);
        int eos;
        int rc;

        if (ret != MPP_OK)
            return (int)RKVC_STATUS_HW;
        if (!frame)
            return until_eos ? (int)RKVC_STATUS_HW : 0;
        if (mpp_frame_get_info_change(frame)) {
            ret = dec->mpi->control(dec->ctx,
                                    MPP_DEC_SET_INFO_CHANGE_READY, NULL);
            mpp_frame_deinit(&frame);
            if (ret != MPP_OK)
                return (int)RKVC_STATUS_HW;
            continue;
        }

        eos = !!mpp_frame_get_eos(frame);
        if (mpp_frame_get_buffer(frame)) {
            rc = emit_mpp_frame(node, frame);
            mpp_frame_deinit(&frame);
            if (rc == 0)
                dec->emitted++;
            if (rc != 0)
                return rc;
        } else {
            mpp_frame_deinit(&frame);
        }
        if (eos)
            return 0;
    }
}

/** 声明端口格式：输入 BITSTREAM/HOST，输出 NV12/DMABUF。 */
static int mpp_dec_configure(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    rkvc_frame_spec input = {0};
    rkvc_frame_spec output = {0};
    (void)diag;

    input.fmt = RKVC_FRAME_FMT_BITSTREAM;
    input.domain = RKVC_MEM_DOMAIN_HOST;
    output.width = dec->request.width;
    output.height = dec->request.height;
    output.fmt = RKVC_FRAME_FMT_NV12;
    output.domain = RKVC_MEM_DOMAIN_DMABUF;
    rkvc_port_set_desired(&node->in_ports[0], &input);
    rkvc_port_set_desired(&node->out_ports[0], &output);
    return 0;
}

/** mpp_init 及依赖编码类型的 control 集中在此，供显式/嗅探两条路径复用。 */
static int decoder_init_locked(struct mpp_decoder *dec) {
    RK_S64 input_timeout = MPP_TIMEOUT_NON_BLOCK;
    RK_S64 output_timeout = MPP_TIMEOUT_NON_BLOCK;
    MppFrameFormat output_format = MPP_FMT_YUV420SP;
    MPP_RET ret;

    ret = mpp_init(dec->ctx, MPP_CTX_DEC, dec->coding);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_SET_INPUT_TIMEOUT,
                                &input_timeout);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT,
                                &output_timeout);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_OUTPUT_FORMAT,
                                &output_format);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    dec->initialized = 1;
    return 0;
}

/** 创建解码器并启用解析器 split 模式；编码仍需从首帧嗅探时
 * 初始化推迟到 process 阶段。 */
static int mpp_dec_open(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    RK_U32 split_mode = 1;
    MPP_RET ret;
    (void)diag;

    /* TRANSCODE 中 codec 字段表达目标编码，输入编码一律首帧嗅探。 */
    if (dec->request.operation == RKVC_OPERATION_DECODE)
        dec->coding = rkvc_to_mpp_coding(dec->request.codec);

    ret = mpp_create(&dec->ctx, &dec->mpi);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE,
                            &split_mode);
    if (ret != MPP_OK) {
        mpp_destroy(dec->ctx);
        dec->ctx = NULL;
        dec->mpi = NULL;
        return (int)RKVC_STATUS_HW;
    }
    if (dec->coding != MPP_VIDEO_CodingUnused)
        return decoder_init_locked(dec);
    return 0;
}

/** 送入一帧码流；首次使用时完成解码器初始化。
 * 输入超时非阻塞：MPP 内部 packet buffer 满（BUFFER_FULL）时先排空
 * 已解码帧释放缓冲再重试，避免 put/get 同线程互等死锁。 */
static int mpp_dec_process(rkvc_node *node, rkvc_frame *input,
                           rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    rkvc_frame_desc desc;
    MppPacket packet = NULL;
    MPP_RET ret;
    /* 时间预算而非次数预算：drain 有产出即重置；无进展满 10s 判硬件假死 */
    struct timespec deadline;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        desc.spec.fmt != RKVC_FRAME_FMT_BITSTREAM || !desc.data || !desc.size)
        return (int)RKVC_STATUS_FORMAT;

    if (!dec->initialized) {
        int rc;
        dec->coding = sniff_annexb_coding(desc.data, desc.size);
        if (dec->coding == MPP_VIDEO_CodingUnused) {
            if (diag)
                rkvc_diag_push(diag, RKVC_STATUS_FORMAT, 3, node->ops->id,
                               "cannot sniff input codec (pass an explicit "
                               "codec for AV1/non-Annex-B streams)");
            return (int)RKVC_STATUS_FORMAT;
        }
        rc = decoder_init_locked(dec);
        if (rc != 0)
            return rc;
    }

    ret = mpp_packet_init(&packet, desc.data, desc.size);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_NOMEM;
    mpp_packet_set_pts(packet, desc.pts);
    mpp_packet_set_dts(packet, desc.dts);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 10;
    for (;;) {
        uint64_t before;
        int rc;
        ret = dec->mpi->decode_put_packet(dec->ctx, packet);
        if (ret == MPP_OK)
            break;
        if (ret != MPP_ERR_BUFFER_FULL) {
            mpp_packet_deinit(&packet);
            return (int)RKVC_STATUS_HW;
        }
        /* 内部缓冲满：先取走已解码帧腾出空间。若下游队列同样满压，
         * drain 内的 emit 会在本线程阻塞等待消费，不会丢失帧。 */
        mpp_packet_deinit(&packet);
        if (mpp_packet_init(&packet, desc.data, desc.size) != MPP_OK)
            return (int)RKVC_STATUS_NOMEM;
        mpp_packet_set_pts(packet, desc.pts);
        mpp_packet_set_dts(packet, desc.dts);
        before = dec->emitted;
        rc = drain_decoder(node, 0);
        if (rc != 0) {
            mpp_packet_deinit(&packet);
            return rc;
        }
        if (dec->emitted != before) {
            /* 有进展：重置假死计时 */
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            deadline = now;
            deadline.tv_sec += 10;
        } else {
            struct timespec now, ts = {0, 1000000}; /* 1ms，官方测试节奏 */
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec &&
                 now.tv_nsec >= deadline.tv_nsec)) {
                mpp_packet_deinit(&packet);
                if (diag)
                    rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                                   "decoder input stays full; hw stall?");
                return (int)RKVC_STATUS_HW;
            }
            nanosleep(&ts, NULL);
        }
    }
    mpp_packet_deinit(&packet);
    return drain_decoder(node, 0);
}

/** 发送 EOS 包并排空全部剩余解码帧。 */
static int mpp_flush(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    RK_S64 output_timeout = 5000;
    MppPacket packet = NULL;
    MPP_RET ret;
    (void)diag;

    if (!dec->initialized)
        return 0; /* 空输入流：无解码器可 flush */
    ret = dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT,
                            &output_timeout);
    if (ret != MPP_OK || mpp_packet_init(&packet, NULL, 0) != MPP_OK)
        return (int)RKVC_STATUS_HW;
    mpp_packet_set_eos(packet);
    ret = dec->mpi->decode_put_packet(dec->ctx, packet);
    mpp_packet_deinit(&packet);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    return drain_decoder(node, 1);
}

/** 销毁 MPP 解码上下文（幂等）。 */
static void mpp_dec_close(rkvc_node *node) {
    struct mpp_decoder *dec = node->priv;
    if (dec && dec->ctx) {
        mpp_destroy(dec->ctx);
        dec->ctx = NULL;
        dec->mpi = NULL;
        dec->initialized = 0;
    }
}

/* ═══════════════════════════ 编码节点 ═══════════════════════════ */

/** 编码节点私有状态。 */
struct mpp_encoder {
    rkvc_request   request;    /**< 请求副本（编码/质量） */
    MppCtx         ctx;        /**< MPP 上下文；open 前为 NULL */
    MppApi        *mpi;        /**< MPP 控制 API */
    MppCodingType  coding;     /**< 目标编码（仅 AVC/HEVC） */
    MppBufferGroup group;      /**< HOST 输入用的内部 DMA 缓冲池 */
    MppEncCfg      cfg;        /**< 保持存活供运行期重调的编码配置 */
    MppEncROICfg   roi_cfg;    /**< 逐帧复用的 ROI 描述符草稿区 */
    MppEncROIRegion roi_regions[RKVC_ROI_MAX_REGIONS]; /**< ROI 区域草稿区 */
    uint32_t       width, height, hor_stride, ver_stride; /**< 协商出的几何参数 */
    MppFrameFormat format;    /**< MPP 输入格式（NV12 / YUV420P） */
    int32_t        applied_bps; /**< 当前已设码率（fixqp 时为 0） */
    uint32_t       applied_gop; /**< 当前已设 GOP */
    int            fixqp;     /**< 固定 QP 模式忽略运行期码率 */
};

/** 接受上游任意已解析格式；声明 BITSTREAM/HOST 输出。 */
static int mpp_enc_configure(rkvc_node *node, rkvc_diag **diag) {
    rkvc_frame_spec input = {0};   /* UNKNOWN：接受上游任意已解析格式 */
    rkvc_frame_spec output = {0};
    (void)diag;

    output.fmt = RKVC_FRAME_FMT_BITSTREAM;
    output.domain = RKVC_MEM_DOMAIN_HOST;
    rkvc_port_set_desired(&node->in_ports[0], &input);
    rkvc_port_set_desired(&node->out_ports[0], &output);
    return 0;
}

/** 协商几何参数、配置码控并初始化编码器；几何未知时推迟到首帧。 */
static int mpp_enc_open(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    const rkvc_frame_spec *in = &node->in_ports[0].fmt;
    MppEncCfg cfg = NULL;
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    /* 输出非阻塞：B 帧重排序/GOP 缓冲期 MPP 不产包，阻塞模式会把
     * worker 卡死在 mpp_cond_wait，与调用方形成三线程互等。 */
    /* 保持同步 task 接口的 frame/buffer 引用语义，但用有限输入超时
     * 杜绝硬件异常时永久卡在 encode_put_frame。 */
    RK_S64 input_timeout = 5000;
    RK_S64 output_timeout = MPP_TIMEOUT_NON_BLOCK;
    MPP_RET ret;
    int64_t bps;

    enc->coding = rkvc_to_mpp_coding(enc->request.codec);
    if (enc->coding != MPP_VIDEO_CodingAVC &&
        enc->coding != MPP_VIDEO_CodingHEVC)
        return (int)RKVC_STATUS_FORMAT;
    if (in->fmt != RKVC_FRAME_FMT_NV12 &&
        in->fmt != RKVC_FRAME_FMT_YUV420P)
        return (int)RKVC_STATUS_FORMAT;

    enc->width = in->width ? in->width : enc->request.width;
    enc->height = in->height ? in->height : enc->request.height;
    /* TRANSCODE 协商时可能只知道像素格式。硬件初始化推迟到首个
     * 解码帧给出真实几何参数之后。 */
    if (!enc->width || !enc->height)
        return 0;
    enc->hor_stride = in->stride ? in->stride : enc->width;
    enc->ver_stride = in->ver_stride ? in->ver_stride : enc->height;
    enc->format = in->fmt == RKVC_FRAME_FMT_NV12 ? MPP_FMT_YUV420SP
                                                 : MPP_FMT_YUV420P;

    ret = mpp_create(&enc->ctx, &enc->mpi);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    if (mpp_enc_cfg_init(&cfg) != MPP_OK)
        goto fail;

    mpp_enc_cfg_set_s32(cfg, "prep:width", (RK_S32)enc->width);
    mpp_enc_cfg_set_s32(cfg, "prep:height", (RK_S32)enc->height);
    mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", (RK_S32)enc->hor_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", (RK_S32)enc->ver_stride);
    mpp_enc_cfg_set_s32(cfg, "prep:format", (RK_S32)enc->format);
    mpp_enc_cfg_set_s32(cfg, "codec:type", (RK_S32)enc->coding);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", MPP_ENC_DEFAULT_FPS);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", MPP_ENC_DEFAULT_FPS);
    mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(cfg, "rc:gop", MPP_ENC_DEFAULT_GOP);

    if (enc->request.quality.qp >= 0) {
        RK_S32 qp = enc->request.quality.qp;
        mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_FIXQP);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_init", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_i", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_p", qp);
        mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_p", qp);
        enc->fixqp = 1;
    } else {
        bps = enc->request.quality.bitrate_bps > 0
                  ? enc->request.quality.bitrate_bps
                  : (int64_t)enc->width * enc->height * 3;
        mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", (RK_S32)bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (RK_S32)(bps * 17 / 16));
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (RK_S32)(bps * 15 / 16));
        enc->applied_bps = (int32_t)bps;
    }
    enc->applied_gop = MPP_ENC_DEFAULT_GOP;

    /* 编码器必须先 mpp_init 再下发配置：MPP_ENC_SET_CFG 依赖已建好的
     * 编码上下文（否则 mpp_control_enc 断言 mEnc 为空）。 */
    if (mpp_init(enc->ctx, MPP_CTX_ENC, enc->coding) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE,
                          &header_mode) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_SET_INPUT_TIMEOUT,
                          &input_timeout) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT,
                          &output_timeout) != MPP_OK) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "mpp encoder init failed");
        mpp_enc_cfg_deinit(cfg);
        goto fail;
    }
    enc->cfg = cfg;
    if (mpp_buffer_group_get_internal(&enc->group,
                                      MPP_BUFFER_TYPE_DMA_HEAP) != MPP_OK &&
        mpp_buffer_group_get_internal(&enc->group,
                                      MPP_BUFFER_TYPE_ION) != MPP_OK) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "mpp buffer group alloc failed");
        goto fail;
    }
    return 0;

fail:
    if (enc->cfg) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = NULL;
    }
    mpp_destroy(enc->ctx);
    enc->ctx = NULL;
    enc->mpi = NULL;
    return (int)RKVC_STATUS_HW;
}

/** 应用逐帧运行时控制（IDR/GOP/码率）；0 表示“保持当前值”。
 * 记录已下发的取值以跳过多余的 SET_CFG 调用。 */
static int enc_apply_control(struct mpp_encoder *enc,
                             const rkvc_encode_control *control) {
    int changed = 0;

    if (!enc || !control)
        return (int)RKVC_STATUS_INVALID;
    if (control->gop_size && control->gop_size != enc->applied_gop) {
        mpp_enc_cfg_set_s32(enc->cfg, "rc:gop", (RK_S32)control->gop_size);
        changed = 1;
    }
    if (!enc->fixqp && control->bitrate_bps > 0 &&
        control->bitrate_bps != enc->applied_bps) {
        int64_t bps = control->bitrate_bps;
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_target", (RK_S32)bps);
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_max", (RK_S32)(bps * 17 / 16));
        mpp_enc_cfg_set_s32(enc->cfg, "rc:bps_min", (RK_S32)(bps * 15 / 16));
        changed = 1;
    }
    if (changed &&
        enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, enc->cfg) != MPP_OK)
        return (int)RKVC_STATUS_HW;
    if (control->force_idr &&
        enc->mpi->control(enc->ctx, MPP_ENC_SET_IDR_FRAME, NULL) != MPP_OK)
        return (int)RKVC_STATUS_HW;
    if (control->gop_size)
        enc->applied_gop = control->gop_size;
    if (!enc->fixqp && control->bitrate_bps > 0)
        enc->applied_bps = control->bitrate_bps;
    return 0;
}

/** 把 rkvc ROI 区域（对齐到 16 像素并裁剪进帧内）转换为挂在帧
 * meta 上的 MPP 附属数据。 */
static int enc_attach_roi(struct mpp_encoder *enc, MppFrame frame,
                          const rkvc_frame_desc *desc) {
    size_t i;
    MppMeta meta;

    if (!desc->roi_region_count)
        return 0;
    if (desc->roi_region_count > RKVC_ROI_MAX_REGIONS)
        return (int)RKVC_STATUS_INVALID;
    memset(enc->roi_regions, 0, sizeof(enc->roi_regions));
    memset(&enc->roi_cfg, 0, sizeof(enc->roi_cfg));
    for (i = 0; i < desc->roi_region_count; ++i) {
        const rkvc_roi_region *src = &desc->roi_regions[i];
        MppEncROIRegion *dst = &enc->roi_regions[i];
        uint32_t left = src->x & ~15u;
        uint32_t top = src->y & ~15u;
        uint64_t right64 = (uint64_t)src->x + src->width;
        uint64_t bottom64 = (uint64_t)src->y + src->height;
        uint32_t right;
        uint32_t bottom;

        right64 = (right64 + 15u) & ~(uint64_t)15u;
        bottom64 = (bottom64 + 15u) & ~(uint64_t)15u;
        right = right64 > enc->width ? enc->width : (uint32_t)right64;
        bottom = bottom64 > enc->height ? enc->height : (uint32_t)bottom64;
        if (left > UINT16_MAX || top > UINT16_MAX || right <= left ||
            bottom <= top || right - left > UINT16_MAX ||
            bottom - top > UINT16_MAX)
            return (int)RKVC_STATUS_FORMAT;
        dst->x = (RK_U16)left;
        dst->y = (RK_U16)top;
        dst->w = (RK_U16)(right - left);
        dst->h = (RK_U16)(bottom - top);
        dst->intra = src->force_intra ? 1 : 0;
        dst->quality = (RK_S16)src->qp_delta;
        dst->qp_area_idx = 0;
        dst->area_map_en = 1;
        dst->abs_qp_en = 0;
    }
    enc->roi_cfg.number = (RK_U32)desc->roi_region_count;
    enc->roi_cfg.regions = enc->roi_regions;
    meta = mpp_frame_get_meta(frame);
    if (!meta || mpp_meta_set_ptr(meta, KEY_ROI_DATA, &enc->roi_cfg) != MPP_OK)
        return (int)RKVC_STATUS_HW;
    return 0;
}

/** HOST 输入按平面拷入 MPP 内部缓冲（处理两侧 stride 差异）。 */
static MppBuffer enc_host_copy(struct mpp_encoder *enc,
                               const rkvc_frame_desc *desc) {
    size_t src_row = desc->spec.stride ? desc->spec.stride
                                       : desc->spec.width;
    size_t src_ver = desc->spec.ver_stride ? desc->spec.ver_stride
                                           : desc->spec.height;
    size_t need = (size_t)enc->hor_stride * enc->ver_stride * 3 / 2;
    MppBuffer buf = NULL;
    unsigned char *dst;
    const unsigned char *src;
    uint32_t y;

    if (mpp_buffer_get(enc->group, &buf, need) != MPP_OK || !buf)
        return NULL;
    dst = mpp_buffer_get_ptr(buf);
    src = desc->data;
    for (y = 0; y < enc->height; ++y)
        memcpy(dst + (size_t)y * enc->hor_stride,
               src + (size_t)y * src_row, enc->width);
    dst += (size_t)enc->hor_stride * enc->ver_stride;
    src += src_row * src_ver;
    for (y = 0; y < enc->height / 2; ++y)
        memcpy(dst + (size_t)y * enc->hor_stride,
               src + (size_t)y * src_row, enc->width);
    return buf;
}

/** DMA-BUF 输入：优先 fd 导入（零拷贝），失败回退 mmap+拷贝。 */
static MppBuffer enc_dmabuf_wrap(struct mpp_encoder *enc,
                                 const rkvc_frame_desc *desc) {
    MppBufferInfo info;
    MppBuffer buf = NULL;

    memset(&info, 0, sizeof(info));
    info.type = MPP_BUFFER_TYPE_DMA_HEAP;
    info.size = desc->size;
    info.fd = desc->fd;
    if (mpp_buffer_import(&buf, &info) == MPP_OK && buf)
        return buf;

#ifdef __linux__
    {
        void *mapped = mmap(NULL, desc->size, PROT_READ, MAP_SHARED,
                            desc->fd, 0);
        if (mapped != MAP_FAILED) {
            rkvc_frame_desc host_view = *desc;
            host_view.data = mapped;
            buf = enc_host_copy(enc, &host_view);
            munmap(mapped, desc->size);
        }
    }
#endif
    return buf;
}

/** 把一个编码后的包从 MPP 拷出并作为 HOST BITSTREAM 帧发出
 * （MppPacket 本体在本调用返回后立即回收）。 */
static int enc_emit_packet(rkvc_node *node, MppPacket packet) {
    size_t len = mpp_packet_get_length(packet);
    void *copy;
    rkvc_frame_desc desc;
    rkvc_frame *out = NULL;
    rkvc_status st;
    int rc = 0;

    if (!len)
        return 0;
    copy = malloc(len);
    if (!copy)
        return (int)RKVC_STATUS_NOMEM;
    memcpy(copy, mpp_packet_get_pos(packet), len);

    rkvc_frame_desc_init(&desc, sizeof(desc));
    desc.spec.fmt = RKVC_FRAME_FMT_BITSTREAM;
    desc.spec.domain = RKVC_MEM_DOMAIN_HOST;
    desc.data = copy;
    desc.size = len;
    desc.pts = mpp_packet_get_pts(packet);
    desc.dts = mpp_packet_get_dts(packet);
    st = rkvc_backend_frame_create(&desc, free, copy, &out);
    if (st != RKVC_STATUS_OK) {
        free(copy);
        return (int)st;
    }
    rc = rkvc_node_emit(node, 0, out);
    if (rc != 0)
        rkvc_frame_release(out);
    return rc;
}

/** 反复调用 encode_get_packet 直到取干；until_eos 时必须等到 EOS 包。
 * 输出超时由调用路径约定：process 路径 NON_BLOCK（NULL=暂无就绪包，
 * 正常返回）；flush 路径有限超时（NULL=等 EOS 超时，HW）。 */
static int enc_drain_packets(rkvc_node *node, int until_eos) {
    struct mpp_encoder *enc = node->priv;

    for (;;) {
        MppPacket packet = NULL;
        MPP_RET ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
        int eos, rc;

        if (ret != MPP_OK) {
            /* legacy async API reports an empty non-blocking output list as
             * MPP_NOK instead of returning MPP_OK with a NULL packet. */
            if (ret == MPP_NOK && !until_eos)
                return 0;
            return (int)RKVC_STATUS_HW;
        }
        if (!packet) {
            if (until_eos)
                return (int)RKVC_STATUS_HW;
            return 0; /* 非阻塞：暂无就绪包 */
        }
        eos = !!mpp_packet_get_eos(packet);
        rc = enc_emit_packet(node, packet);
        mpp_packet_deinit(&packet);
        if (rc != 0)
            return rc;
        if (eos || !until_eos)
            return 0;
    }
}

/** 编码一帧：应用运行时控制、准备缓冲（拷贝或导入）、挂 ROI
 * 附属数据、提交并排出已就绪的包。 */
static int mpp_enc_process(rkvc_node *node, rkvc_frame *input,
                           rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    rkvc_frame_desc desc;
    MppFrame frame = NULL;
    MppBuffer buf = NULL;
    MPP_RET ret;
    int rc = 0;
    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;
    if (desc.spec.fmt != RKVC_FRAME_FMT_NV12 &&
        desc.spec.fmt != RKVC_FRAME_FMT_YUV420P)
        return (int)RKVC_STATUS_FORMAT;
    if (!enc->ctx) {
        if (!desc.spec.width || !desc.spec.height)
            return (int)RKVC_STATUS_FORMAT;
        node->in_ports[0].fmt = desc.spec;
        rc = mpp_enc_open(node, diag);
        if (rc != 0)
            return rc;
    }
    if ((desc.spec.width && desc.spec.width != enc->width) ||
        (desc.spec.height && desc.spec.height != enc->height))
        return (int)RKVC_STATUS_FORMAT;

    rc = enc_apply_control(enc, &desc.encode);
    if (rc != 0) {
        if (diag)
            rkvc_diag_push(diag, (rkvc_status)rc, 3, node->ops->id,
                           "runtime encoder control failed");
        return rc;
    }

    if (desc.spec.domain == RKVC_MEM_DOMAIN_DMABUF) {
        if (desc.fd < 0 || desc.size == 0)
            return (int)RKVC_STATUS_FORMAT;
        buf = enc_dmabuf_wrap(enc, &desc);
    } else {
        if (!desc.data)
            return (int)RKVC_STATUS_FORMAT;
        buf = enc_host_copy(enc, &desc);
    }
    if (!buf)
        return (int)RKVC_STATUS_NOMEM;

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK) {
        mpp_buffer_put(buf);
        return (int)RKVC_STATUS_HW;
    }
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->format);
    mpp_frame_set_buffer(frame, buf);
    mpp_frame_set_pts(frame, desc.pts);
    rc = enc_attach_roi(enc, frame, &desc);
    if (rc == 0)
        rc = enc->mpi->encode_put_frame(enc->ctx, frame) == MPP_OK
                 ? 0 : (int)RKVC_STATUS_HW;
    mpp_frame_deinit(&frame);
    if (rc != 0) {
        mpp_buffer_put(buf);
        return rc;
    }
    rc = enc_drain_packets(node, 0);
    mpp_buffer_put(buf);
    return rc;
}

/** 提交 EOS 帧并排空全部剩余编码包。 */
static int mpp_enc_flush(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    MppFrame frame = NULL;
    MPP_RET ret;
    int rc;
    (void)diag;

    if (!enc->ctx)
        return 0; /* 空输入流：惰性编码器从未初始化 */
    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->format);
    mpp_frame_set_eos(frame, 1);
    rc = enc->mpi->encode_put_frame(enc->ctx, frame) == MPP_OK
             ? 0 : (int)RKVC_STATUS_HW;
    mpp_frame_deinit(&frame);
    if (rc != 0)
        return rc;
    /* EOS 已成功入队后有限阻塞等待尾包，排干重排序缓冲。 */
    {
        RK_S64 output_timeout = 5000;
        if (enc->mpi->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT,
                              &output_timeout) != MPP_OK)
            return (int)RKVC_STATUS_HW;
    }
    return enc_drain_packets(node, 1);
}

/** 释放配置、MPP 上下文与缓冲池（幂等）。 */
static void mpp_enc_close(rkvc_node *node) {
    struct mpp_encoder *enc = node->priv;
    if (!enc)
        return;
    if (enc->cfg) {
        mpp_enc_cfg_deinit(enc->cfg);
        enc->cfg = NULL;
    }
    if (enc->ctx) {
        mpp_destroy(enc->ctx);
        enc->ctx = NULL;
        enc->mpi = NULL;
    }
    if (enc->group) {
        mpp_buffer_group_put(enc->group);
        enc->group = NULL;
    }
}

/* ═══════════════════ 节点生命周期 / 工厂 / 注册 ═══════════════════ */

/** 通用节点析构：close 后释放 priv 与端口数组。 */
static void mpp_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    if (node->ops->close)
        node->ops->close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops mpp_dec_ops = {
    "mpp.decode", mpp_dec_configure, mpp_dec_open, mpp_dec_process,
    mpp_flush, mpp_dec_close, mpp_destroy_node,
};

static const rkvc_node_ops mpp_enc_ops = {
    "mpp.encode", mpp_enc_configure, mpp_enc_open, mpp_enc_process,
    mpp_enc_flush, mpp_enc_close, mpp_destroy_node,
};

/** 解码工厂门控：DECODE 且编码已知（或 AUTO）；TRANSCODE 恒匹配，
 * 因为输入编码从首帧嗅探。 */
static int mpp_dec_matches(rkvc_operation op, rkvc_codec codec,
                           const rkvc_device_caps *caps) {
    (void)caps;
    if (op == RKVC_OPERATION_TRANSCODE)
        return 1; /* 输入编码在首帧嗅探 */
    return op == RKVC_OPERATION_DECODE &&
           (codec == RKVC_CODEC_AUTO || codec == RKVC_CODEC_H264 ||
            codec == RKVC_CODEC_HEVC || codec == RKVC_CODEC_AV1);
}

/** 编码工厂门控：ENCODE/TRANSCODE 且目标为 H.264 或 HEVC。 */
static int mpp_enc_matches(rkvc_operation op, rkvc_codec codec,
                           const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_ENCODE || op == RKVC_OPERATION_TRANSCODE) &&
           (codec == RKVC_CODEC_H264 || codec == RKVC_CODEC_HEVC);
}

/** 加分项：realtime 策略下优先选择本硬件路径。 */
static int mpp_score(const rkvc_request *request,
                     const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_REALTIME ? 100 : 50;
}

/** 两个工厂共用的 create；按 stage 选择编码/解码 ops。 */
static rkvc_node *mpp_node_create(const rkvc_node_factory *factory,
                                  const rkvc_request *request,
                                  void *create_ctx) {
    int is_encode = factory->stage == RKVC_NODE_STAGE_ENCODE;
    rkvc_node *node = calloc(1, sizeof(*node));
    void *priv = calloc(1, is_encode ? sizeof(struct mpp_encoder)
                                     : sizeof(struct mpp_decoder));
    (void)create_ctx;
    if (!node || !priv) {
        free(node);
        free(priv);
        return NULL;
    }
    node->ops = is_encode ? &mpp_enc_ops : &mpp_dec_ops;
    node->priv = priv;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        mpp_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = is_encode ? "video" : "bitstream";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = is_encode ? "bitstream" : "video";
    if (is_encode)
        ((struct mpp_encoder *)priv)->request = *request;
    else
        ((struct mpp_decoder *)priv)->request = *request;
    return node;
}

/** 设备探测：要求存在 MPP 服务节点且支持 AVC。 */
static int mpp_probe(const rkvc_device_caps *caps, void *probe_ctx,
                     rkvc_diag **diag) {
    (void)caps;
    (void)probe_ctx;
    (void)diag;
    if (access("/dev/mpp_service", R_OK | W_OK) != 0 &&
        access("/dev/mpp-service", R_OK | W_OK) != 0)
        return -errno;
    if (mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAVC) != MPP_OK &&
        mpp_check_support_format(MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK)
        return -ENOTSUP;
    return 0;
}

/** 工厂表：解码与编码各一项。 */
static const rkvc_node_factory mpp_factories[] = {
    {
        .id = "mpp.decode",
        .backend_id = "mpp",
        .stage = RKVC_NODE_STAGE_DECODE,
        .priority = 1000,
        .matches = mpp_dec_matches,
        .score = mpp_score,
        .create = mpp_node_create,
    },
    {
        .id = "mpp.encode",
        .backend_id = "mpp",
        .stage = RKVC_NODE_STAGE_ENCODE,
        .priority = 1000,
        .matches = mpp_enc_matches,
        .score = mpp_score,
        .create = mpp_node_create,
    },
};

/** factories 回调：返回静态表。 */
static const rkvc_node_factory *mpp_factory_list(void *probe_ctx,
                                                 size_t *count) {
    (void)probe_ctx;
    *count = sizeof(mpp_factories) / sizeof(mpp_factories[0]);
    return mpp_factories;
}

/** 经 rkvc_backend_query() 导出的后端描述符。 */
static const rkvc_backend mpp_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "mpp",
    .capability_flags = RKVC_BACKEND_CAP_MPP_DECODE |
                        RKVC_BACKEND_CAP_MPP_ENCODE,
    .probe = mpp_probe,
    .factories = mpp_factory_list,
};

/** DSO 入口：返回静态后端描述符。 */
const rkvc_backend *rkvc_backend_query(void) { return &mpp_backend; }
