/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* MPP link stub for container load-smoke tests ONLY. Every entry fails
 * closed (no device here); the plugin loader only needs its symbols to
 * resolve so dlopen succeeds and factory registration is exercised.
 * Board builds always link the real librockchip_mpp instead. */
#include <stddef.h>

#include "rk_mpi.h"

MPP_RET mpp_create(MppCtx *ctx, MppApi **mpi) {
    if (ctx)
        *ctx = NULL;
    if (mpi)
        *mpi = NULL;
    return MPP_NOK;
}

MPP_RET mpp_destroy(MppCtx ctx) {
    (void)ctx;
    return MPP_OK;
}

MPP_RET mpp_init(MppCtx ctx, MppCtxType type, MppCodingType coding) {
    (void)ctx;
    (void)type;
    (void)coding;
    return MPP_NOK;
}

MPP_RET mpp_check_support_format(MppCtxType type, MppCodingType coding) {
    (void)type;
    (void)coding;
    return MPP_NOK;
}

MPP_RET mpp_buffer_import_with_tag(MppBufferGroup group, MppBufferInfo *info,
                                   MppBuffer *buffer, const char *tag,
                                   const char *caller) {
    (void)group;
    (void)info;
    (void)buffer;
    (void)tag;
    (void)caller;
    return MPP_NOK;
}

MPP_RET mpp_buffer_get_with_tag(MppBufferGroup group, MppBuffer *buffer,
                                size_t size, const char *tag,
                                const char *caller) {
    (void)group;
    (void)buffer;
    (void)size;
    (void)tag;
    (void)caller;
    return MPP_NOK;
}

MPP_RET mpp_buffer_put_with_caller(MppBuffer buffer, const char *caller) {
    (void)buffer;
    (void)caller;
    return MPP_NOK;
}

MPP_RET mpp_buffer_group_get(MppBufferGroup *group, MppBufferType type,
                             MppBufferMode mode, const char *tag,
                             const char *caller) {
    (void)group;
    (void)type;
    (void)mode;
    (void)tag;
    (void)caller;
    return MPP_NOK;
}

MPP_RET mpp_buffer_group_put(MppBufferGroup group) {
    (void)group;
    return MPP_OK;
}

void *mpp_buffer_get_ptr_with_caller(MppBuffer buffer, const char *caller) {
    (void)buffer;
    (void)caller;
    return NULL;
}

int mpp_buffer_get_fd_with_caller(MppBuffer buffer, const char *caller) {
    (void)buffer;
    (void)caller;
    return -1;
}

size_t mpp_buffer_get_size_with_caller(MppBuffer buffer, const char *caller) {
    (void)buffer;
    (void)caller;
    return 0;
}

MPP_RET mpp_enc_cfg_init(MppEncCfg *cfg) {
    if (cfg)
        *cfg = NULL;
    return MPP_NOK;
}

MPP_RET mpp_enc_cfg_deinit(MppEncCfg cfg) {
    (void)cfg;
    return MPP_OK;
}

MPP_RET mpp_enc_cfg_set_s32(MppEncCfg cfg, const char *name, RK_S32 val) {
    (void)cfg;
    (void)name;
    (void)val;
    return MPP_NOK;
}

MPP_RET mpp_frame_init(MppFrame *frame) {
    if (frame)
        *frame = NULL;
    return MPP_NOK;
}

MPP_RET mpp_frame_deinit(MppFrame *frame) {
    (void)frame;
    return MPP_OK;
}

MppBuffer mpp_frame_get_buffer(const MppFrame frame) {
    (void)frame;
    return NULL;
}

MppFrameFormat mpp_frame_get_fmt(MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_width(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_height(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_hor_stride(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_ver_stride(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_S64 mpp_frame_get_pts(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_S64 mpp_frame_get_dts(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_errinfo(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_discard(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_info_change(const MppFrame frame) {
    (void)frame;
    return 0;
}

RK_U32 mpp_frame_get_eos(const MppFrame frame) {
    (void)frame;
    return 0;
}

MppMeta mpp_frame_get_meta(const MppFrame frame) {
    (void)frame;
    return NULL;
}

void mpp_frame_set_buffer(MppFrame frame, MppBuffer buffer) {
    (void)frame;
    (void)buffer;
}

void mpp_frame_set_width(MppFrame frame, RK_U32 width) {
    (void)frame;
    (void)width;
}

void mpp_frame_set_height(MppFrame frame, RK_U32 height) {
    (void)frame;
    (void)height;
}

void mpp_frame_set_hor_stride(MppFrame frame, RK_U32 stride) {
    (void)frame;
    (void)stride;
}

void mpp_frame_set_ver_stride(MppFrame frame, RK_U32 stride) {
    (void)frame;
    (void)stride;
}

void mpp_frame_set_fmt(MppFrame frame, MppFrameFormat fmt) {
    (void)frame;
    (void)fmt;
}

void mpp_frame_set_pts(MppFrame frame, RK_S64 pts) {
    (void)frame;
    (void)pts;
}

void mpp_frame_set_eos(MppFrame frame, RK_U32 eos) {
    (void)frame;
    (void)eos;
}

MPP_RET mpp_meta_set_ptr(MppMeta meta, MppMetaKey key, void *val) {
    (void)meta;
    (void)key;
    (void)val;
    return MPP_NOK;
}

MPP_RET mpp_packet_init(MppPacket *packet, void *data, size_t size) {
    (void)data;
    (void)size;
    if (packet)
        *packet = NULL;
    return MPP_NOK;
}

MPP_RET mpp_packet_deinit(MppPacket *packet) {
    (void)packet;
    return MPP_OK;
}

void *mpp_packet_get_pos(const MppPacket packet) {
    (void)packet;
    return NULL;
}

size_t mpp_packet_get_length(const MppPacket packet) {
    (void)packet;
    return 0;
}

RK_S64 mpp_packet_get_pts(const MppPacket packet) {
    (void)packet;
    return 0;
}

RK_S64 mpp_packet_get_dts(const MppPacket packet) {
    (void)packet;
    return 0;
}

RK_U32 mpp_packet_get_eos(const MppPacket packet) {
    (void)packet;
    return 0;
}

void mpp_packet_set_pts(MppPacket packet, RK_S64 pts) {
    (void)packet;
    (void)pts;
}

void mpp_packet_set_dts(MppPacket packet, RK_S64 dts) {
    (void)packet;
    (void)dts;
}

MPP_RET mpp_packet_set_eos(MppPacket packet) {
    (void)packet;
    return MPP_NOK;
}
