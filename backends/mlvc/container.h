/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file container.h
 * @brief .mlvc 容器的无状态头部读写与流式 demux。
 *
 * 容器线格式（全部小端，单一格式，无版本协商）：
 *   头（64B）：magic "MLVC1" + format 字节 0x02@5 + w@8 + h@12 +
 *     fps_num@16 + fps_den@20 + qp@24 + frame_count@28 +
 *     iframe_period@32 + ltr_start_idx@36 + ltr_period@40 +
 *     hdr_flags@44 + target_bitrate_bps@48 + reserved@52..63
 *   帧记录（16B）：4B payload size + 4B q_index + 4B flags +
 *     4B reserved + payload
 *
 * 语义：
 * - qp@24 为会话基准 q_index；逐帧实际 q_index 在记录头（动态码控）。
 * - q_index == MLVC_QINDEX_DROPPED 且 size == 0 表示丢帧（解码端
 *   重复上一输出帧）。
 * - 帧型由记录 flags 显式携带（KEYFRAME/LTR_MARK/LTR_RECOVERY），
 *   解码端不依赖会话参数推导；头里的周期字段为信息性。
 * - frame_count 头字段恒 0（读端以 EOF 为准）。
 */

#ifndef RKVC_BACKEND_MLVC_CONTAINER_H
#define RKVC_BACKEND_MLVC_CONTAINER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MLVC_MAGIC      "MLVC1"
#define MLVC_MAGIC_LEN  5
#define MLVC_FORMAT     2    /**< @5 固定格式标识字节，不符即拒绝 */
#define MLVC_HDR_SIZE   64
#define MLVC_REC_SIZE   16
#define MLVC_MAX_FRAME_BYTES (64u * 1024u * 1024u)

/* 记录 flags */
#define MLVC_REC_KEYFRAME     0x1u /**< IDR：解码端清空 DPB */
#define MLVC_REC_LTR_MARK     0x2u /**< 解码后 feature 存入 LTR 槽 */
#define MLVC_REC_LTR_RECOVERY 0x4u /**< 本帧以 LTR 槽为参考 */

/** 丢帧哨兵（对齐官方 Q_INDEX_DROP_SENTINEL）。 */
#define MLVC_QINDEX_DROPPED (-1)

/* 头 flags（@44，信息性） */
#define MLVC_HDR_FLAG_PROACTIVE_LTR 0x1u
#define MLVC_HDR_FLAG_CBR           0x2u

/** 容器头字段（解析后）。 */
typedef struct {
    uint32_t width, height;
    uint32_t fps_num, fps_den;
    uint32_t qp;               /**< 会话基准 q_index */
    uint32_t frame_count;      /**< 恒 0 */
    uint32_t iframe_period;    /**< IDR 周期；0 = 仅首帧 */
    uint32_t ltr_start_idx;    /**< 首个 LTR 帧序 */
    uint32_t ltr_period;       /**< LTR 周期；0 = 关闭 */
    uint32_t flags;            /**< MLVC_HDR_FLAG_* */
    uint32_t target_bitrate_bps; /**< CBR 目标码率（信息性） */
} mlvc_container_header;

/** 序列化 64B 头（frame_count 写 0）。 */
void mlvc_container_write_header(uint8_t *out64,
                                 const mlvc_container_header *hdr);

/**
 * @brief 解析容器头。
 * @param in 缓冲。
 * @param size 可用字节数。
 * @param out 解析结果。
 * @return >0 头部字节数（64）；0 缓冲不足；-1 magic/格式标识不符。
 */
int mlvc_container_parse_header(const uint8_t *in, size_t size,
                                mlvc_container_header *out);

/** 序列化一条帧记录头（16B：size + q_index + flags + reserved）。 */
void mlvc_container_write_record(uint8_t *out16, uint32_t payload_size,
                                 int32_t q_index, uint32_t flags);

/**
 * @brief 流式 demux 状态：累积 source 块，按记录边界切帧。
 *
 * 推入任意块（append），随后反复 next() 取出完整帧载荷。
 */
typedef struct {
    uint8_t  *buf;
    size_t    len, cap;
    mlvc_container_header hdr;
    int have_header;
    int parse_error;
    uint32_t frames_emitted;
} mlvc_demuxer;

/** 初始化 demuxer（buf 未分配）。 */
void mlvc_demux_init(mlvc_demuxer *d);

/** 释放内部缓冲。 */
void mlvc_demux_free(mlvc_demuxer *d);

/**
 * @brief 追加一块码流字节。
 * @return 0 成功；负 rkvc_status（NOMEM/FORMAT）。
 */
int mlvc_demux_append(mlvc_demuxer *d, const uint8_t *data, size_t size);

/** 已累积未消费字节数。 */
size_t mlvc_demux_consumed(const mlvc_demuxer *d);

/**
 * @brief 取出下一帧。
 *
 * @param out_data 帧载荷指针（指向内部缓冲，调用方立即消费或拷贝；
 *                 任何后续 append/next/consume 调用前有效）；丢帧为 NULL。
 * @param out_size 帧载荷字节数；丢帧为 0。
 * @param out_q_index 帧 q_index；丢帧为 MLVC_QINDEX_DROPPED。
 * @param out_flags MLVC_REC_* 组合。
 * @return 1 取到一帧（含丢帧标记帧）；0 缓冲不足；-1 格式错误。
 */
int mlvc_demux_next(mlvc_demuxer *d, const uint8_t **out_data,
                    size_t *out_size, int32_t *out_q_index,
                    uint32_t *out_flags);

/** 消费 n 字节（须在 next 返回 1 后调用：16 + payload_size）。 */
void mlvc_demux_consume(mlvc_demuxer *d, size_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_CONTAINER_H */
