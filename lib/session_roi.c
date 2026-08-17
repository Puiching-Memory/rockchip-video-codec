/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file session_roi.c
 * @brief ROI 配置（MPP 硬路径见 node_mpp_enc / rkmppenc）。
 */

#include "internal.h"

rkvc_err rkvc_session_set_roi(rkvc_session *session,
                              const rkvc_roi_rect *rects, int count)
{
    if (!session)
        return RKVC_ERR_INVALID;
    if (count < 0 || count > RKVC_ROI_MAX)
        return RKVC_ERR_INVALID;
    if (count > 0 && !rects)
        return RKVC_ERR_INVALID;

    int denom = session->desc.enc_scale_denom;
    int ew = session->desc.width;
    int eh = session->desc.height;
    if (denom > 1) {
        ew = ew > 0 ? (ew / denom) & ~1 : 0;
        eh = eh > 0 ? (eh / denom) & ~1 : 0;
    }

    for (int i = 0; i < count; i++) {
        if (rects[i].x < 0 || rects[i].y < 0 ||
            rects[i].w <= 0 || rects[i].h <= 0)
            return RKVC_ERR_INVALID;
        /* 已知编码分辨率时拒绝越界；宽高仍为 0 则只做正矩形检查。 */
        if (ew > 0 && eh > 0) {
            if (rects[i].x > ew - rects[i].w ||
                rects[i].y > eh - rects[i].h)
                return RKVC_ERR_INVALID;
        }
    }

    pthread_mutex_lock(&session->lock);
    session->roi_count = count;
    if (count > 0)
        memcpy(session->rois, rects, (size_t)count * sizeof(rkvc_roi_rect));
    pthread_mutex_unlock(&session->lock);
    return RKVC_OK;
}

rkvc_err rkvc_session_clear_roi(rkvc_session *session)
{
    return rkvc_session_set_roi(session, NULL, 0);
}
