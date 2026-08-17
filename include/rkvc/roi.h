/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file roi.h
 * @brief 编码侧 ROI（区域相对 QP / 可选 force_intra；不绑定检测模型）。
 *
 * 仅 **H.264 / HEVC（MPP）** 生效：经 `AV_FRAME_DATA_REGIONS_OF_INTEREST` +
 * 帧 metadata `rkvc_roi_force_intra`，由 `rkmppenc` 桥到 MPP `KEY_ROI_DATA`。
 * **SVT-AV1 忽略 ROI**（无硬 ROI 桥接；不做像素级 fallback）。
 *
 * 矩形须落在当前编码分辨率内（已知宽高时；`enc_scale_denom>1` 时相对缩放后尺寸）。
 * 矩形建议 16 像素对齐（MPP 会再对齐）。
 */

#ifndef RKVC_ROI_H
#define RKVC_ROI_H

#include "rkvc/types.h"
#include "rkvc/session.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 单 Session 最多 ROI 矩形数（与 MPP legacy / 产品上限对齐）。 */
#define RKVC_ROI_MAX 8

/**
 * @brief 感兴趣区域矩形（像素坐标，相对编码分辨率）。
 *
 * 字段用 `x,y,w,h`（应用/检测框友好）；内部再转 FFmpeg/MPP 坐标。
 * `qp_offset`：相对 QP；负=更清晰（多码），正=更省码；0=不改 QP。
 */
typedef struct {
    int x;           /**< 左上角 x（像素） */
    int y;           /**< 左上角 y（像素） */
    int w;           /**< 宽（像素，>0） */
    int h;           /**< 高（像素，>0） */
    int qp_offset;   /**< 相对 QP；负=更清晰，正=更省码；0=不改 QP */
    int force_intra; /**< 非 0：该区强制帧内（仅 MPP 硬路径生效） */
} rkvc_roi_rect;

/**
 * @brief 设置 ROI 列表（覆盖旧配置）。
 *
 * @param session 会话。
 * @param rects   矩形数组；`count==0` 或 `rects==NULL` 等效清空。
 * @param count   矩形个数，0…`RKVC_ROI_MAX`。
 * @return `RKVC_OK` 或 `RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_session_set_roi(rkvc_session *session,
                              const rkvc_roi_rect *rects, int count);

/** @brief 清空 ROI。 */
rkvc_err rkvc_session_clear_roi(rkvc_session *session);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_ROI_H */
