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

    for (int i = 0; i < count; i++) {
        if (rects[i].w <= 0 || rects[i].h <= 0)
            return RKVC_ERR_INVALID;
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
