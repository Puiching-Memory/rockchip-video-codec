/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * Self-contained Rockchip MPP backend for the versioned rkvc ABI:
 * H.264/HEVC/AV1 hardware decode + H.264/HEVC hardware encode.
 *
 * mpp.decode: explicit request codec is used directly; AUTO (and TRANSCODE,
 * where the codec field names the target) is resolved by sniffing Annex-B
 * parameter sets on the first bitstream frame, then MPP is initialized
 * lazily. mpp.encode accepts NV12/YUV420P HOST frames (copied into an MPP
 * buffer) or DMA-BUF frames (fd import with copy fallback) and emits
 * BITSTREAM host frames with SPS/PPS on every IDR.
 *
 * Compressed/tiled MPP layouts need an explicit DRM modifier contract;
 * this backend deliberately exposes only linear frames.
 */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

#include <rk_mpi.h>

#define MPP_ENC_DEFAULT_FPS 30
#define MPP_ENC_DEFAULT_GOP 60

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

static MppCodingType rkvc_to_mpp_coding(rkvc_codec codec) {
    switch (codec) {
    case RKVC_CODEC_H264: return MPP_VIDEO_CodingAVC;
    case RKVC_CODEC_HEVC: return MPP_VIDEO_CodingHEVC;
    case RKVC_CODEC_AV1:  return MPP_VIDEO_CodingAV1;
    default:              return MPP_VIDEO_CodingUnused;
    }
}

/* Scan an Annex-B stream for parameter sets: H.264 SPS / HEVC VPS.
 * AV1 has no start codes and must be requested explicitly. */
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

struct mpp_decoder {
    rkvc_request  request;
    MppCtx        ctx;
    MppApi       *mpi;
    MppCodingType coding;      /* MPP_VIDEO_CodingUnused = 待首帧嗅探 */
    int           initialized;
};

static int emit_mpp_frame(rkvc_node *node, MppFrame frame) {
    MppBuffer buffer = mpp_frame_get_buffer(frame);
    MppFrameFormat format = mpp_frame_get_fmt(frame);
    rkvc_frame_desc desc;
    rkvc_frame *output = NULL;
    rkvc_status st;
    int rc;

    /* Compressed/tiled MPP layouts need an explicit DRM modifier contract.
     * The first portable backend deliberately exposes only linear frames. */
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

    /* The core duplicates and owns the fd. MppFrame can therefore be released
     * immediately, and an output frame may safely outlive its backend DSO. */
    st = rkvc_backend_frame_create_dmabuf(&desc, &output);
    if (st != RKVC_STATUS_OK)
        return (int)st;
    rc = rkvc_node_emit(node, 0, output);
    if (rc != 0)
        rkvc_frame_release(output);
    return rc;
}

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
            if (rc != 0)
                return rc;
        } else {
            mpp_frame_deinit(&frame);
        }
        if (eos)
            return 0;
    }
}

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

/* mpp_init 及依赖编码类型的 control 集中在此，供显式/嗅探两条路径复用。 */
static int decoder_init_locked(struct mpp_decoder *dec) {
    RK_S64 input_timeout = MPP_TIMEOUT_BLOCK;
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

static int mpp_dec_process(rkvc_node *node, rkvc_frame *input,
                           rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    rkvc_frame_desc desc;
    MppPacket packet = NULL;
    MPP_RET ret;

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
    ret = dec->mpi->decode_put_packet(dec->ctx, packet);
    mpp_packet_deinit(&packet);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    return drain_decoder(node, 0);
}

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

struct mpp_encoder {
    rkvc_request   request;
    MppCtx         ctx;
    MppApi        *mpi;
    MppCodingType  coding;
    MppBufferGroup group;
    uint32_t       width, height, hor_stride, ver_stride;
    MppFrameFormat format;
};

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

static int mpp_enc_open(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    const rkvc_frame_spec *in = &node->in_ports[0].fmt;
    MppEncCfg cfg = NULL;
    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    RK_S64 block = MPP_TIMEOUT_BLOCK;
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
    if (!enc->width || !enc->height)
        return (int)RKVC_STATUS_FORMAT;
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
    } else {
        bps = enc->request.quality.bitrate_bps > 0
                  ? enc->request.quality.bitrate_bps
                  : (int64_t)enc->width * enc->height * 3;
        mpp_enc_cfg_set_s32(cfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", (RK_S32)bps);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", (RK_S32)(bps * 17 / 16));
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", (RK_S32)(bps * 15 / 16));
    }

    /* 编码器必须先 mpp_init 再下发配置：MPP_ENC_SET_CFG 依赖已建好的
     * 编码上下文（否则 mpp_control_enc 断言 mEnc 为空）。 */
    if (mpp_init(enc->ctx, MPP_CTX_ENC, enc->coding) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_ENC_SET_CFG, cfg) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_ENC_SET_HEADER_MODE,
                          &header_mode) != MPP_OK ||
        enc->mpi->control(enc->ctx, MPP_SET_OUTPUT_TIMEOUT,
                          &block) != MPP_OK) {
        if (diag)
            rkvc_diag_push(diag, RKVC_STATUS_HW, 3, node->ops->id,
                           "mpp encoder init failed");
        mpp_enc_cfg_deinit(cfg);
        goto fail;
    }
    mpp_enc_cfg_deinit(cfg);
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
    mpp_destroy(enc->ctx);
    enc->ctx = NULL;
    enc->mpi = NULL;
    return (int)RKVC_STATUS_HW;
}

/* HOST 输入按平面拷入 MPP 内部缓冲（处理两侧 stride 差异）。 */
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

/* DMA-BUF 输入：优先 fd 导入（零拷贝），失败回退 mmap+拷贝。 */
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

static int enc_emit_packet(rkvc_node *node, MppPacket packet) {
    size_t len = mpp_packet_get_length(packet);
    void *copy;
    rkvc_frame_desc desc;
    rkvc_frame *out = NULL;
    rkvc_status st;
    int rc;

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

static int enc_drain_packets(rkvc_node *node, int until_eos) {
    struct mpp_encoder *enc = node->priv;

    for (;;) {
        MppPacket packet = NULL;
        MPP_RET ret = enc->mpi->encode_get_packet(enc->ctx, &packet);
        int eos, rc;

        if (ret != MPP_OK)
            return (int)RKVC_STATUS_HW;
        if (!packet)
            return 0; /* 防御：阻塞输出模式下不应出现空包 */
        eos = !!mpp_packet_get_eos(packet);
        rc = enc_emit_packet(node, packet);
        mpp_packet_deinit(&packet);
        if (rc != 0)
            return rc;
        if (eos || !until_eos)
            return 0;
    }
}

static int mpp_enc_process(rkvc_node *node, rkvc_frame *input,
                           rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    rkvc_frame_desc desc;
    MppFrame frame = NULL;
    MppBuffer buf = NULL;
    MPP_RET ret;
    int rc;
    (void)diag;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK)
        return (int)RKVC_STATUS_FORMAT;

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
    if (ret == MPP_OK) {
        mpp_frame_set_width(frame, enc->width);
        mpp_frame_set_height(frame, enc->height);
        mpp_frame_set_hor_stride(frame, enc->hor_stride);
        mpp_frame_set_ver_stride(frame, enc->ver_stride);
        mpp_frame_set_fmt(frame, enc->format);
        mpp_frame_set_buffer(frame, buf);
        mpp_frame_set_pts(frame, desc.pts);
        ret = enc->mpi->encode_put_frame(enc->ctx, frame);
        mpp_frame_deinit(&frame);
    }
    if (ret != MPP_OK) {
        mpp_buffer_put(buf);
        return (int)RKVC_STATUS_HW;
    }
    rc = enc_drain_packets(node, 0);
    mpp_buffer_put(buf);
    return rc;
}

static int mpp_enc_flush(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_encoder *enc = node->priv;
    MppFrame frame = NULL;
    MPP_RET ret;
    (void)diag;

    ret = mpp_frame_init(&frame);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    mpp_frame_set_width(frame, enc->width);
    mpp_frame_set_height(frame, enc->height);
    mpp_frame_set_hor_stride(frame, enc->hor_stride);
    mpp_frame_set_ver_stride(frame, enc->ver_stride);
    mpp_frame_set_fmt(frame, enc->format);
    mpp_frame_set_eos(frame, 1);
    ret = enc->mpi->encode_put_frame(enc->ctx, frame);
    mpp_frame_deinit(&frame);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    return enc_drain_packets(node, 1);
}

static void mpp_enc_close(rkvc_node *node) {
    struct mpp_encoder *enc = node->priv;
    if (!enc)
        return;
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

static int mpp_dec_matches(rkvc_operation op, rkvc_codec codec,
                           const rkvc_device_caps *caps) {
    (void)caps;
    if (op == RKVC_OPERATION_TRANSCODE)
        return 1; /* 输入编码在首帧嗅探 */
    return op == RKVC_OPERATION_DECODE &&
           (codec == RKVC_CODEC_AUTO || codec == RKVC_CODEC_H264 ||
            codec == RKVC_CODEC_HEVC || codec == RKVC_CODEC_AV1);
}

static int mpp_enc_matches(rkvc_operation op, rkvc_codec codec,
                           const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_ENCODE || op == RKVC_OPERATION_TRANSCODE) &&
           (codec == RKVC_CODEC_H264 || codec == RKVC_CODEC_HEVC);
}

static int mpp_score(const rkvc_request *request,
                     const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_REALTIME ? 100 : 50;
}

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

static const rkvc_node_factory *mpp_factory_list(void *probe_ctx,
                                                 size_t *count) {
    (void)probe_ctx;
    *count = sizeof(mpp_factories) / sizeof(mpp_factories[0]);
    return mpp_factories;
}

static const rkvc_backend mpp_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "mpp",
    .capability_flags = RKVC_BACKEND_CAP_MPP_DECODE |
                        RKVC_BACKEND_CAP_MPP_ENCODE,
    .probe = mpp_probe,
    .factories = mpp_factory_list,
};

const rkvc_backend *rkvc_backend_query(void) { return &mpp_backend; }
