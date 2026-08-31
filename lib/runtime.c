/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file runtime.c
 * @brief 进程级 Session 资源配额。
 */

#include "internal.h"

static pthread_mutex_t s_rt_lock = PTHREAD_MUTEX_INITIALIZER;
static rkvc_runtime_quota s_quota;
static int s_sessions;
static int s_enc_sessions;
static int s_npu_sessions;

rkvc_err rkvc_runtime_set_quota(const rkvc_runtime_quota *quota)
{
    pthread_mutex_lock(&s_rt_lock);
    if (!quota)
        memset(&s_quota, 0, sizeof(s_quota));
    else
        s_quota = *quota;
    pthread_mutex_unlock(&s_rt_lock);
    return RKVC_OK;
}

rkvc_err rkvc_runtime_get_stats(rkvc_runtime_stats *out)
{
    if (!out)
        return RKVC_ERR_INVALID;
    pthread_mutex_lock(&s_rt_lock);
    out->quota = s_quota;
    out->sessions = s_sessions;
    out->enc_sessions = s_enc_sessions;
    out->npu_sessions = s_npu_sessions;
    pthread_mutex_unlock(&s_rt_lock);
    return RKVC_OK;
}

static int template_has_enc(rkvc_pipeline_template t)
{
    return t == RKVC_TEMPLATE_FILE_ENCODE ||
           t == RKVC_TEMPLATE_FILE_TRANSCODE ||
           t == RKVC_TEMPLATE_LIVE_CAPTURE ||
           t == RKVC_TEMPLATE_LIVE_TRANSCODE ||
           t == RKVC_TEMPLATE_AV1_STORAGE ||
           t == RKVC_TEMPLATE_MLVC_STORAGE;
}

static int template_has_npu(const rkvc_pipeline_desc *d)
{
    if (d->post_upscale_algo == RKVC_UPSCALE_AI_SR)
        return 1;
    if (d->template_id == RKVC_TEMPLATE_MLVC_STORAGE)
        return 1;
    if (d->codec == RKVC_CODEC_MLVC || d->policy == RKVC_POLICY_NEURAL)
        return 1;
    if (d->mlvc_enc_model_path || d->mlvc_dec_model_path)
        return 1;
    return 0;
}

rkvc_err rkvc_runtime_try_register(const rkvc_pipeline_desc *desc,
                                   int *out_flags)
{
    if (!desc || !out_flags)
        return RKVC_ERR_INVALID;

    int flags = RKVC_RT_FLAG_SESSION;
    if (template_has_enc(desc->template_id))
        flags |= RKVC_RT_FLAG_ENC;
    if (template_has_npu(desc))
        flags |= RKVC_RT_FLAG_NPU;

    pthread_mutex_lock(&s_rt_lock);
    if (s_quota.max_sessions > 0 && s_sessions >= s_quota.max_sessions) {
        pthread_mutex_unlock(&s_rt_lock);
        return RKVC_ERR_AGAIN;
    }
    if ((flags & RKVC_RT_FLAG_ENC) && s_quota.max_enc_sessions > 0 &&
        s_enc_sessions >= s_quota.max_enc_sessions) {
        pthread_mutex_unlock(&s_rt_lock);
        return RKVC_ERR_AGAIN;
    }
    if ((flags & RKVC_RT_FLAG_NPU) && s_quota.max_npu_sessions > 0 &&
        s_npu_sessions >= s_quota.max_npu_sessions) {
        pthread_mutex_unlock(&s_rt_lock);
        return RKVC_ERR_AGAIN;
    }

    s_sessions++;
    if (flags & RKVC_RT_FLAG_ENC)
        s_enc_sessions++;
    if (flags & RKVC_RT_FLAG_NPU)
        s_npu_sessions++;
    pthread_mutex_unlock(&s_rt_lock);

    *out_flags = flags;
    return RKVC_OK;
}

void rkvc_runtime_unregister(int flags)
{
    if (!(flags & RKVC_RT_FLAG_SESSION))
        return;
    pthread_mutex_lock(&s_rt_lock);
    if (s_sessions > 0)
        s_sessions--;
    if ((flags & RKVC_RT_FLAG_ENC) && s_enc_sessions > 0)
        s_enc_sessions--;
    if ((flags & RKVC_RT_FLAG_NPU) && s_npu_sessions > 0)
        s_npu_sessions--;
    pthread_mutex_unlock(&s_rt_lock);
}
