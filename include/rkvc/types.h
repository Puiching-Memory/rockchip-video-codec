/**
 * @file types.h
 * @brief 公共类型定义：错误码、像素格式、基本配置结构。
 */

#ifndef RKVC_TYPES_H
#define RKVC_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 错误码 ───────────────────────────────────────────────────────── */

typedef enum {
    RKVC_OK              =  0,  /**< 成功 */
    RKVC_ERR_NOMEM       = -1,  /**< 内存分配失败 */
    RKVC_ERR_INVALID     = -2,  /**< 参数无效 */
    RKVC_ERR_NOT_FOUND   = -3,  /**< 编解码器或设备未找到 */
    RKVC_ERR_IO          = -4,  /**< I/O 错误 */
    RKVC_ERR_HW          = -5,  /**< 硬件加速初始化失败 */
    RKVC_ERR_EOF         = -6,  /**< 流结束 */
    RKVC_ERR_AGAIN       = -7,  /**< 需要更多输入或输出缓冲区满 */
    RKVC_ERR_MUX         = -8,  /**< 封装器错误 */
    RKVC_ERR_INTERNAL    = -9,  /**< 内部 FFmpeg 错误 */
    RKVC_ERR_PERMISSION  = -10, /**< 设备节点权限不足 */
    RKVC_ERR_FORMAT      = -11, /**< 输入数据格式不匹配 */
} rkvc_err;

/* ── 像素格式 ─────────────────────────────────────────────────────── */

typedef enum {
    RKVC_PIX_FMT_NV12    = 0,   /**< NV12 (默认, VPU 原生) */
    RKVC_PIX_FMT_YUV420P = 1,   /**< YUV420P planar */
    RKVC_PIX_FMT_NV16    = 2,   /**< NV16 (4:2:2 semi-planar) */
    RKVC_PIX_FMT_P010    = 3,   /**< P010 (10-bit 4:2:0) */
} rkvc_pix_fmt;

/* ── 编码质量预设 ──────────────────────────────────────────────────── */

typedef enum {
    RKVC_PRESET_FAST     = 0,   /**< 速度优先 */
    RKVC_PRESET_MEDIUM   = 1,   /**< 均衡 */
    RKVC_PRESET_SLOW     = 2,   /**< 质量优先 */
} rkvc_preset;

/* ── 码率控制模式 ──────────────────────────────────────────────────── */

/** 与 MPP / FFmpeg rkmppenc rc_mode 数值一致 */
typedef enum {
    RKVC_RC_VBR = 0,            /**< 可变码率 */
    RKVC_RC_CBR = 1,            /**< 恒定码率 */
    RKVC_RC_CQP = 2,            /**< 固定 QP (MPP FIXQP) */
} rkvc_rc_mode;

#define RKRC_VBR RKVC_RC_VBR
#define RKRC_CBR RKVC_RC_CBR
#define RKRC_CQP RKVC_RC_CQP

/* ── 解码后上采样算法（RGA 硬件插值）──────────────────────────── */

typedef enum {
    RKVC_UPSCALE_NONE = 0,      /**< 不做后处理上采样 */
    RKVC_UPSCALE_NEAREST,       /**< 最近邻 (RGA) */
    RKVC_UPSCALE_BILINEAR,      /**< 双线性 (RGA) */
    RKVC_UPSCALE_BICUBIC,       /**< 双三次 (RGA) */
    RKVC_UPSCALE_AI_SR,         /**< RKVC 神经网络超分 */
} rkvc_upscale_algo;

#ifdef __cplusplus
}
#endif

#endif /* RKVC_TYPES_H */
