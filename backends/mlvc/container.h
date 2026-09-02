/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file container.h
 * @brief .mlvc 容器的无状态头部读写与流式 demux。
 *
 * 容器线格式（全部小端）：
 *   32B 头：magic "MLVC1" + version(1B@5) + w@8 + h@12 + fps_num@16 +
 *           fps_den@20 + qp@24 + frame_count@28
 *   每帧记录：4B payload size + 1B keyframe + 3B pad + payload
 *
 * 0.4 适配：编码节点每帧输出完整记录（首帧附带头部），文件直写即得
 * 合法 .mlvc；frame_count 头字段只在 demux 校验用，编码侧不再回填
 * （读端以 EOF 为准）。
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
#define MLVC_VERSION    1
#define MLVC_HDR_SIZE   32
#define MLVC_REC_SIZE   8
#define MLVC_MAX_FRAME_BYTES (64u * 1024u * 1024u)

/** 容器头字段（解析后）。 */
typedef struct {
    uint32_t width, height;
    uint32_t fps_num, fps_den;
    uint32_t qp;
    uint32_t frame_count;
} mlvc_container_header;

/** 序列化 32B 头（frame_count 写入 0；见文件头注释）。 */
void mlvc_container_write_header(uint8_t *out32,
                                 uint32_t width, uint32_t height,
                                 uint32_t fps_num, uint32_t fps_den,
                                 uint32_t qp);

/**
 * @brief 解析 32B 头。
 * @return 0 成功；-1 magic/version 不符。
 */
int mlvc_container_parse_header(const uint8_t *in32,
                                mlvc_container_header *out);

/** 序列化一条帧记录头（8B：size + keyframe + pad）。 */
void mlvc_container_write_record(uint8_t *out8, uint32_t payload_size,
                                 int keyframe);

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

/**
 * @brief 取出下一帧。
 *
 * @param out_data 帧载荷指针（指向内部缓冲，调用方立即消费或拷贝；
 *                 任何后续 append/next/consume 调用前有效）。
 * @param out_size 帧载荷字节数。
 * @param out_keyframe 帧关键帧标志。
 * @return 1 取到一帧；0 缓冲不足（需再 append 或已到尾部）；-1 格式错误。
 */
int mlvc_demux_next(mlvc_demuxer *d, const uint8_t **out_data,
                    size_t *out_size, int *out_keyframe);

/** 消费 next() 返回帧的记录字节（REC+payload），前移内部缓冲。 */
void mlvc_demux_consume(mlvc_demuxer *d, size_t bytes);

/** 已消费字节数（调试/校验用）。 */
size_t mlvc_demux_consumed(const mlvc_demuxer *d);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_BACKEND_MLVC_CONTAINER_H */
