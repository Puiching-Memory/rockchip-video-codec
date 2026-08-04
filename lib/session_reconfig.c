/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session_reconfig.c
 * @brief 运行中码率 / GOP / IDR 热切换（帧边界生效）。
 */

#include "internal.h"

rkvc_err rkvc_session_set_bitrate(rkvc_session *session, int64_t bitrate)
{
    if (!session || bitrate <= 0)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&session->lock);
    session->desc.bitrate = bitrate;
    session->reconfig_pending |= RKVC_RECONFIG_BITRATE;
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

rkvc_err rkvc_session_set_gop(rkvc_session *session, int gop_size)
{
    if (!session || gop_size < 1)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&session->lock);
    session->desc.gop_size = gop_size;
    session->reconfig_pending |= RKVC_RECONFIG_GOP;
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

rkvc_err rkvc_session_request_idr(rkvc_session *session)
{
    if (!session)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&session->lock);
    session->reconfig_pending |= RKVC_RECONFIG_IDR;
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

rkvc_err rkvc_session_reconfigure(rkvc_session *session,
                                  const rkvc_reconfig_desc *desc)
{
    if (!session || !desc)
        return RKVC_ERR_INVALID;
    if (desc->flags == 0)
        return RKVC_OK;

    if ((desc->flags & RKVC_RECONFIG_BITRATE) && desc->bitrate <= 0)
        return RKVC_ERR_INVALID;
    if ((desc->flags & RKVC_RECONFIG_GOP) && desc->gop_size < 1)
        return RKVC_ERR_INVALID;

    pthread_mutex_lock(&session->lock);
    if (desc->flags & RKVC_RECONFIG_BITRATE)
        session->desc.bitrate = desc->bitrate;
    if (desc->flags & RKVC_RECONFIG_GOP)
        session->desc.gop_size = desc->gop_size;
    session->reconfig_pending |= desc->flags;
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

/**
 * 在下一帧送入编码器前应用挂起的热切换。
 * MPP：写 AVCodecContext + 可选 force_idr；SVT：仅同步 desc（无运行时 RC）。
 * 若 MPP 应用失败，恢复 pending 位以便下次重试。
 */
rkvc_err rkvc_session_apply_reconfig(rkvc_session *s, int *force_idr)
{
    if (!s)
        return RKVC_ERR_INVALID;
    if (force_idr)
        *force_idr = 0;

    pthread_mutex_lock(&s->lock);
    unsigned pending = s->reconfig_pending;
    int64_t bitrate = s->desc.bitrate;
    int gop = s->desc.gop_size;
    if (pending)
        s->reconfig_pending = 0;
    pthread_mutex_unlock(&s->lock);

    if (!pending)
        return RKVC_OK;

    if (force_idr && (pending & RKVC_RECONFIG_IDR))
        *force_idr = 1;

    if (s->enc) {
        rkvc_err err = rkvc_mpp_enc_apply_rc(s->enc, bitrate, gop);
        if (err != RKVC_OK) {
            pthread_mutex_lock(&s->lock);
            s->reconfig_pending |= pending;
            pthread_mutex_unlock(&s->lock);
            if (force_idr)
                *force_idr = 0;
            return err;
        }
    }
    /* SVT：desc 已更新；运行中改参需重建编码器，此处不报错。 */
    return RKVC_OK;
}
