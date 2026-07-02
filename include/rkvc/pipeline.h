/**
 * @file pipeline.h
 * @brief 管线描述与预置模板。
 */

#ifndef RKVC_PIPELINE_H
#define RKVC_PIPELINE_H

#include "rkvc/types.h"
#include "rkvc/policy.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RKVC_TEMPLATE_FILE_ENCODE = 0,
    RKVC_TEMPLATE_FILE_DECODE,
    RKVC_TEMPLATE_FILE_TRANSCODE,
    RKVC_TEMPLATE_LIVE_CAPTURE,
    RKVC_TEMPLATE_AV1_STORAGE,
} rkvc_pipeline_template;

typedef struct rkvc_pipeline_desc {
    rkvc_pipeline_template template_id;
    rkvc_policy            policy;
    rkvc_codec             codec;

    int            width;
    int            height;
    int            fps_num;
    int            fps_den;
    int64_t        bitrate;
    rkvc_pix_fmt   pixel_format;
    int            gop_size;
    int            b_frames;
    int            low_latency;
    int            queue_depth;   /**< 端口队列深度，默认 3 */
    rkvc_rc_mode   rc_mode;       /**< MPP 码率控制，默认 CBR */
    int            qp_init;       /**< 固定 QP 初值，-1 表示编码器默认 */

    const char    *input_path;    /**< 文件解码/转码输入 */
    const char    *output_path;   /**< 文件编码/转码输出 */

    /**
     * 编码前下采样分母（1=全分辨率编码；2=宽高各减半后编码）。
     * width/height 仍为显示/参考分辨率；解码后可配合 post_upscale_algo 还原。
     */
    int            enc_scale_denom;
    rkvc_upscale_algo post_upscale_algo; /**< 解码后上采样算法 */
    const char    *post_upscale_rkvc_model_path; /**< AI_SR 时必填：RKNN 超分模型路径 */

    /** SVT-AV1 level_of_parallelism（0=按 CPU 核数自动，1–6 手动） */
    int            svt_lp;
    /** SVT-AV1 实时调优（0=关，1=开；适合低延迟，preset 最低 7） */
    int            svt_rtc;

    /**
     * FFmpeg 选项串（key=val:key2=val2），用于 demux / decoder / encoder。
     * 例如 MPP 编码器额外参数：qp_max=48:qp_min=10
     */
    const char    *codec_opts;
} rkvc_pipeline_desc;

rkvc_pipeline_desc rkvc_pipeline_desc_defaults(void);
rkvc_err rkvc_pipeline_from_template(rkvc_pipeline_template tmpl,
                                     rkvc_pipeline_desc *desc);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_PIPELINE_H */
