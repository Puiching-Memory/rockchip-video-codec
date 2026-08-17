/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * lib/rans.h — rANS 熵编解码器（纯 C 实现）。
 *
 * 完整移植自 Microsoft msrtc_rans（mlvc/packages/msrtc_rans，C++17 模板），
 * 覆盖全部功能：
 *   - 两种 rANS 变体：RansByte（uint32_t state / uint8_t unit）和 Rans64（uint64_t / uint32_t）
 *   - PMF 表 → 预计算编码符号表（定点倒数，消除 Put 中的除法）
 *   - PMF 表 → 解码 CDF 累积频率表（二分查找符号）
 *   - 流式编码（多 coder 写入同一流）+ Flush 产出码流
 *   - 流式解码（Open → 多次 Decode）
 *   - 一次性 Encode/Decode（缓冲版本）
 *   - Bypass 编解码（超出符号范围的离群值）
 *   - 可增长堆缓冲（HeapResizableBuffer 等价）
 *   - EOF / 码流完整性校验
 *
 * 算法来源：ryg_rans / msrtc_rans（MIT License, Copyright (c) Microsoft Corporation）。
 */

#ifndef RKVC_RANS_H
#define RKVC_RANS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 错误码 ──────────────────────────────────────────────────────── */

typedef enum {
    RKVC_RANS_OK             =  0,
    RKVC_RANS_ERR_PMF        = -1,  /* 无效 PMF 数据 */
    RKVC_RANS_ERR_PARAMS     = -2,  /* 无效参数值 */
    RKVC_RANS_ERR_STREAM     = -3,  /* 无效/截断码流 */
    RKVC_RANS_ERR_STATE      = -4,  /* 未初始化或状态错误 */
} rkvc_rans_err;

/* ── rANS 变体 ───────────────────────────────────────────────────── */

typedef enum {
    RKVC_RANS_BYTE = 0,   /* uint32_t state, uint8_t  unit — StateBits=31, LowerBound=1<<23 */
    RKVC_RANS_64   = 1,   /* uint64_t state, uint32_t unit — StateBits=63, LowerBound=1<<31 */
} rkvc_rans_variant;

/* ── 预计算编码符号（消除 Put 中的除法）────────────────────────── */

typedef struct {
    uint32_t x_max_hi;        /* freq << (min(StateBits, 31) - scale_bits) */
    uint32_t freq_rcp_shift;  /* 定点倒数移位量 */
    uint32_t freq_rcp;        /* 定点倒数频率（32-bit；Rans64 使用 64-bit 组合）*/
    uint32_t freq_rcp_hi;     /* 仅 Rans64：倒数高 32 位 */
    uint32_t freq_cmpl;       /* (1<<scale_bits) - freq */
    uint32_t bias;            /* 编码偏置 */
} rkvc_rans_enc_sym;

/* ── 概率分布描述 ────────────────────────────────────────────────── */

typedef struct {
    int32_t value_offset;     /* 零值符号偏移（pmf_offsets[i]）*/
    int32_t bypass_sentinel;  /* 最后一个可编码符号索引（pmf_lengths[i] - 1）*/
    size_t  enc_sym_offset;   /* 在编码符号表中的起始偏移（无间隙）*/
    size_t  dec_cdf_offset;   /* 在 CDF 表中的起始偏移（含间隙）*/
} rkvc_rans_dist;

/* ── 熵编码器/解码器（同一结构同时持有编码和解码表）────────────── */

typedef struct {
    rkvc_rans_variant variant;
    int               symbol_bits;     /* 符号编码位数（如 16）*/
    int               bypass_bits;     /* bypass 编码位数（如 2）*/
    uint32_t          bypass_max_value;/* (1<<bypass_bits) - 1 */

    /* 分布描述 */
    rkvc_rans_dist   *dists;
    size_t            num_dists;

    /* 编码器：预计算符号表（从 PMF 直接映射，无间隙）*/
    rkvc_rans_enc_sym *enc_syms;
    size_t             num_enc_syms;

    /* 解码器：CDF 累积频率表（含每分布一个尾项，有间隙）*/
    uint32_t          *cdf_table;
    size_t             cdf_table_size;

    int                initialized;
} rkvc_rans_coder;

/**
 * @brief 从 PMF 表初始化熵编解码器（同时构建编码符号表和解码 CDF 表）。
 *
 * @param coder       输出结构（调用方栈分配或堆分配）。
 * @param variant     rANS 变体。
 * @param pmf_lengths 每分布的符号数（含 bypass 哨兵）。
 * @param num_lengths pmf_lengths 元素数。
 * @param pmf_offsets 每分布的零值偏移。
 * @param num_offsets pmf_offsets 元素数（必须 == num_lengths）。
 * @param pmf_table   拼接的 PMF 频率表。
 * @param table_size  pmf_table 元素数。
 * @param symbol_bits 符号编码位数（2..MaxScaleBits）。
 * @param bypass_bits bypass 编码位数（2..MaxScaleBits）。
 * @return RKVC_RANS_OK 或负错误码。
 */
int rkvc_rans_coder_init(rkvc_rans_coder *coder, rkvc_rans_variant variant,
                         const int32_t *pmf_lengths, size_t num_lengths,
                         const int32_t *pmf_offsets, size_t num_offsets,
                         const int32_t *pmf_table, size_t table_size,
                         int symbol_bits, int bypass_bits);

/** @brief 释放熵编解码器内部动态分配。不释放 coder 本身。 */
void rkvc_rans_coder_free(rkvc_rans_coder *coder);

/* ── 流式编码器 ──────────────────────────────────────────────────── */
/*
 * 编码顺序与 C++ 一致：数据逆序写入（从末尾到开头），
 * Flush 时写入 state 字节，最终码流为 [ptr, end) 字节范围。
 *
 * 典型用法：
 *   rkvc_rans_enc_stream s;
 *   rkvc_rans_enc_stream_init(&s, RKVC_RANS_BYTE);
 *   rkvc_rans_enc_stream_encode(&s, &gaussian, s1, y1, n);
 *   rkvc_rans_enc_stream_encode(&s, &gaussian, s0, y0, n);
 *   rkvc_rans_enc_stream_encode(&s, &bitest,  zi, z,  n);
 *   size_t sz; const uint8_t *out = rkvc_rans_enc_stream_flush(&s, &sz);
 */

typedef struct {
    rkvc_rans_variant variant;
    uint64_t  state;        /* rANS 状态（RansByte 仅用低 32 位）*/
    uint8_t  *buf;          /* 动态增长缓冲起始 */
    uint8_t  *ptr;          /* 当前写入位置（递减方向）*/
    uint8_t  *end;          /* 缓冲结束 */
    size_t    cap;          /* 当前容量（字节）*/
    int       flushed;
    int       oom;          /* enc_grow 失败后禁止再写 */
} rkvc_rans_enc_stream;

/** @brief 初始化流式编码器。initial_capacity 为初始缓冲字节数（最小 512）。 */
void rkvc_rans_enc_stream_init(rkvc_rans_enc_stream *s,
                               rkvc_rans_variant variant,
                               size_t initial_capacity);

/**
 * @brief 向流中编码一组值。
 *
 * 数据逆序编码（与 C++ 实现一致）。indices 和 values 长度必须相同。
 * 可对同一流使用不同 coder 调用多次（gaussian + bitest）。
 *
 * @return RKVC_RANS_OK 或负错误码。
 */
int rkvc_rans_enc_stream_encode(rkvc_rans_enc_stream *s,
                                const rkvc_rans_coder *coder,
                                const int32_t *indices,
                                const int32_t *values, size_t count);

/**
 * @brief Flush 并返回码流指针。
 *
 * @param out_size 输出码流字节数。
 * @return 指向码流的指针（在 stream 被 free 前有效）。NULL 表示错误。
 */
const uint8_t *rkvc_rans_enc_stream_flush(rkvc_rans_enc_stream *s,
                                          size_t *out_size);

/** @brief 重置流式编码器（回到 init 后状态，缓冲复用）。 */
void rkvc_rans_enc_stream_reset(rkvc_rans_enc_stream *s);

/** @brief 释放流式编码器内部缓冲。 */
void rkvc_rans_enc_stream_free(rkvc_rans_enc_stream *s);

/* ── 流式解码器 ──────────────────────────────────────────────────── */

typedef struct {
    rkvc_rans_variant variant;
    uint64_t       state;
    const uint8_t *ptr;
    const uint8_t *end;
    int            opened;
} rkvc_rans_dec_stream;

/** @brief 初始化流式解码器（不打开码流）。 */
void rkvc_rans_dec_stream_init(rkvc_rans_dec_stream *s,
                               rkvc_rans_variant variant);

/**
 * @brief 打开码流并读取初始 state。
 * @return RKVC_RANS_OK 或负错误码（码流过短/无效）。
 */
int rkvc_rans_dec_stream_open(rkvc_rans_dec_stream *s,
                              const uint8_t *data, size_t size);

/**
 * @brief 从流中解码一组值。
 *
 * @param values  输出解码值（调用方分配）。
 * @param indices 输入分布索引（选择每元素的概率分布）。
 * @param count   元素数。
 * @return RKVC_RANS_OK 或负错误码。
 */
int rkvc_rans_dec_stream_decode(rkvc_rans_dec_stream *s,
                                const rkvc_rans_coder *coder,
                                int32_t *values,
                                const int32_t *indices, size_t count);

/** @brief 检查是否到达码流末尾（state == LowerBound 且 ptr == end）。 */
int rkvc_rans_dec_stream_check_eof(const rkvc_rans_dec_stream *s);

/** @brief 关闭码流（重置到未打开状态）。 */
void rkvc_rans_dec_stream_close(rkvc_rans_dec_stream *s);

/* ── 一次性 API（缓冲版本）──────────────────────────────────────── */

/**
 * @brief 一次性编码：创建内部流，编码全部数据，Flush 后返回码流。
 *
 * @param out     输出码流指针（内部 malloc，调用方负责 free）。
 * @param out_sz  输出码流字节数。
 * @return RKVC_RANS_OK 或负错误码。
 */
int rkvc_rans_encode(const rkvc_rans_coder *coder,
                     const int32_t *indices,
                     const int32_t *values, size_t count,
                     uint8_t **out, size_t *out_sz);

/**
 * @brief 一次性解码：打开码流，解码全部数据，校验 EOF。
 *
 * @return RKVC_RANS_OK 或负错误码。
 */
int rkvc_rans_decode(const rkvc_rans_coder *coder,
                     int32_t *values,
                     const int32_t *indices, size_t count,
                     const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_RANS_H */
