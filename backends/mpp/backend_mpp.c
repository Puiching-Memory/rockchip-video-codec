/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/** Self-contained Rockchip MPP H.264 decoder backend for the rkvc 0.4 ABI. */

#include "rkvc/backend.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <rk_mpi.h>

struct mpp_decoder {
    rkvc_request request;
    MppCtx ctx;
    MppApi *mpi;
};

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

static int mpp_configure(rkvc_node *node, rkvc_diag **diag) {
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

static int mpp_open(rkvc_node *node, rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    RK_U32 split_mode = 1;
    MppFrameFormat output_format = MPP_FMT_YUV420SP;
    RK_S64 input_timeout = MPP_TIMEOUT_BLOCK;
    RK_S64 output_timeout = MPP_TIMEOUT_NON_BLOCK;
    MPP_RET ret;
    (void)diag;

    ret = mpp_create(&dec->ctx, &dec->mpi);
    if (ret != MPP_OK)
        return (int)RKVC_STATUS_HW;
    ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_PARSER_SPLIT_MODE,
                            &split_mode);
    if (ret == MPP_OK)
        ret = mpp_init(dec->ctx, MPP_CTX_DEC, MPP_VIDEO_CodingAVC);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_SET_INPUT_TIMEOUT,
                                &input_timeout);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_SET_OUTPUT_TIMEOUT,
                                &output_timeout);
    if (ret == MPP_OK)
        ret = dec->mpi->control(dec->ctx, MPP_DEC_SET_OUTPUT_FORMAT,
                                &output_format);
    if (ret != MPP_OK) {
        mpp_destroy(dec->ctx);
        dec->ctx = NULL;
        dec->mpi = NULL;
        return (int)RKVC_STATUS_HW;
    }
    return 0;
}

static int mpp_process(rkvc_node *node, rkvc_frame *input,
                       rkvc_diag **diag) {
    struct mpp_decoder *dec = node->priv;
    rkvc_frame_desc desc;
    MppPacket packet = NULL;
    MPP_RET ret;
    (void)diag;

    if (rkvc_frame_get_desc(input, &desc) != RKVC_STATUS_OK ||
        desc.spec.fmt != RKVC_FRAME_FMT_BITSTREAM || !desc.data || !desc.size)
        return (int)RKVC_STATUS_FORMAT;
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

static void mpp_close(rkvc_node *node) {
    struct mpp_decoder *dec = node->priv;
    if (dec && dec->ctx) {
        mpp_destroy(dec->ctx);
        dec->ctx = NULL;
        dec->mpi = NULL;
    }
}

static void mpp_destroy_node(rkvc_node *node) {
    if (!node)
        return;
    mpp_close(node);
    free(node->priv);
    free(node->in_ports);
    free(node->out_ports);
    free(node);
}

static const rkvc_node_ops mpp_ops = {
    "mpp.decode.h264", mpp_configure, mpp_open, mpp_process, mpp_flush,
    mpp_close, mpp_destroy_node,
};

static int mpp_matches(rkvc_operation op, rkvc_codec codec,
                       const rkvc_device_caps *caps) {
    (void)caps;
    return (op == RKVC_OPERATION_DECODE || op == RKVC_OPERATION_TRANSCODE) &&
           codec == RKVC_CODEC_H264;
}

static int mpp_score(const rkvc_request *request,
                     const rkvc_device_caps *caps, void *create_ctx) {
    (void)caps;
    (void)create_ctx;
    return request->policy == RKVC_POLICY_REALTIME ? 100 : 50;
}

static rkvc_node *mpp_create_node(const rkvc_node_factory *factory,
                                  const rkvc_request *request,
                                  void *create_ctx) {
    rkvc_node *node = calloc(1, sizeof(*node));
    struct mpp_decoder *dec = calloc(1, sizeof(*dec));
    (void)factory;
    (void)create_ctx;
    if (!node || !dec) {
        free(node);
        free(dec);
        return NULL;
    }
    dec->request = *request;
    node->ops = &mpp_ops;
    node->priv = dec;
    node->in_ports = calloc(1, sizeof(*node->in_ports));
    node->out_ports = calloc(1, sizeof(*node->out_ports));
    if (!node->in_ports || !node->out_ports) {
        mpp_destroy_node(node);
        return NULL;
    }
    node->in_count = 1;
    node->in_ports[0].name = "bitstream";
    node->in_ports[0].is_input = 1;
    node->out_count = 1;
    node->out_ports[0].name = "video";
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
    return mpp_check_support_format(MPP_CTX_DEC, MPP_VIDEO_CodingAVC) == MPP_OK
               ? 0 : -ENOTSUP;
}

static const rkvc_node_factory mpp_factories[] = {{
    .id = "mpp.decode.h264",
    .backend_id = "mpp",
    .stage = RKVC_NODE_STAGE_DECODE,
    .priority = 1000,
    .matches = mpp_matches,
    .score = mpp_score,
    .create = mpp_create_node,
}};

static const rkvc_node_factory *mpp_factory_list(void *probe_ctx,
                                                 size_t *count) {
    (void)probe_ctx;
    *count = sizeof(mpp_factories) / sizeof(mpp_factories[0]);
    return mpp_factories;
}

static const rkvc_backend mpp_backend = {
    .abi_version = RKVC_ABI_VERSION,
    .id = "mpp",
    .capability_flags = RKVC_BACKEND_CAP_MPP_DECODE,
    .probe = mpp_probe,
    .factories = mpp_factory_list,
};

const rkvc_backend *rkvc_backend_query(void) { return &mpp_backend; }
