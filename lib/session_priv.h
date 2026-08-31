/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session_priv.h
 * @brief session.c / session_scale.c / session_run.c 之间的内部接口。
 */

#ifndef RKVC_SESSION_PRIV_H
#define RKVC_SESSION_PRIV_H

#include "internal.h"

rkvc_err session_enc_size(const rkvc_session *s, int *ew, int *eh);
rkvc_err session_downscale_for_encode(rkvc_session *s, rkvc_buffer *frame,
                                      rkvc_buffer **out);
int session_needs_post_upscale(const rkvc_session *s);
int session_uses_ai_sr(const rkvc_session *s);
int session_can_rga_upscale_dmabuf(const rkvc_session *s,
                                   const rkvc_buffer *frame);
int session_can_ai_sr_dmabuf(const rkvc_session *s, const rkvc_buffer *frame);
rkvc_err session_frame_to_host(const rkvc_session *s, rkvc_buffer *frame,
                               rkvc_buffer **host);
rkvc_err session_ensure_rknn_sr(rkvc_session *s);
rkvc_err session_apply_post_upscale(rkvc_session *s, rkvc_buffer *host,
                                    rkvc_buffer **out);
rkvc_err session_write_nv12_frame(FILE *fp, const AVFrame *f);
rkvc_err session_write_nv12_buffer(FILE *fp, const rkvc_buffer *buf);
rkvc_err session_open_live_decoder(rkvc_session *s, rkvc_codec input_codec);

/** LIVE_TRANSCODE 后台线程入口。 */
void *rkvc_session_live_worker(void *opaque);

#endif /* RKVC_SESSION_PRIV_H */
