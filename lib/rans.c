/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */
/*
 * lib/rans.c — rANS 熵编解码器完整 C 实现。
 *
 * 移植自 msrtc_rans（rans.h + EntropyCoder.cpp），覆盖 RansByte / Rans64 双变体、
 * 预计算编码符号（定点倒数除法）、CDF 二分解码、bypass 编解码、流式 + 一次性 API。
 *
 * 算法来源：ryg_rans / msrtc_rans（MIT License, Microsoft Corporation）。
 */

#include "rans.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ════════════════════════════════════════════════════════════════════ */
/*  内部常量                                                            */
/* ════════════════════════════════════════════════════════════════════ */

/* RansByte: uint32_t state, uint8_t unit */
#define RANS_BYTE_STATE_BITS    31
#define RANS_BYTE_MAX_SCALE     30   /* min(STATE_BITS-1, 32) = min(30,32) */
#define RANS_BYTE_LOWER_BOUND   (1u << (RANS_BYTE_STATE_BITS - 8))  /* 1<<23 */
#define RANS_BYTE_UNIT_SIZE     1    /* sizeof(uint8_t) */
#define RANS_BYTE_UNITS_PER_STATE  4  /* sizeof(uint32_t)/sizeof(uint8_t) */

/* Rans64: uint64_t state, uint32_t unit */
#define RANS_64_STATE_BITS      63
#define RANS_64_MAX_SCALE       32   /* min(STATE_BITS-1, 32) = min(62,32) */
#define RANS_64_LOWER_BOUND     (1ULL << (RANS_64_STATE_BITS - 32))  /* 1<<31 */
#define RANS_64_UNIT_SIZE       4    /* sizeof(uint32_t) */
#define RANS_64_UNITS_PER_STATE 2    /* sizeof(uint64_t)/sizeof(uint32_t) */

/* ── 64-bit 乘法高位（Rans64 Quotient 用）────────────────────────── */

static inline uint64_t mul64hi(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t prod = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)(prod >> 64);
#else
    /* Fallback：拆分乘法（理论可用，但 aarch64 有 __uint128__）*/
    uint64_t a_lo = (uint32_t)a, a_hi = a >> 32;
    uint64_t b_lo = (uint32_t)b, b_hi = b >> 32;
    uint64_t lo = a_lo * b_lo;
    uint64_t mid1 = a_hi * b_lo;
    uint64_t mid2 = a_lo * b_hi;
    uint64_t hi = a_hi * b_hi;
    uint64_t mid = mid1 + mid2 + (lo >> 32);
    return hi + (mid >> 32);
#endif
}

/* ════════════════════════════════════════════════════════════════════ */
/*  预计算编码符号构造（RansEncSymbol 构造函数）                       */
/* ════════════════════════════════════════════════════════════════════ */

static void enc_sym_init_byte(rkvc_rans_enc_sym *s,
                               uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint32_t scale = 1u << scale_bits;

    s->x_max_hi = freq << (RANS_BYTE_STATE_BITS - scale_bits);

    if (freq > 1) {
        /* Alverson 倒数除法 */
        uint32_t shift = 1;
        while (freq > (1u << shift))
            shift++;
        /* bits = sizeof(uint32_t)*8 = 32 */
        uint64_t nom = (1ULL << (shift + 32 - 1)) + (freq - 1);
        s->freq_rcp = (uint32_t)(nom / freq);
        s->freq_rcp_shift = shift - 1 + 32;  /* 额外 +32（非 uint64_t state）*/
        s->bias = start;
    } else {
        /* freq == 1：rcp = ~0, shift = 0 */
        s->freq_rcp = 0xFFFFFFFFu;
        s->freq_rcp_shift = 0 + 32;
        s->bias = start + scale - 1;
    }
    s->freq_cmpl = scale - freq;
}

static void enc_sym_init_64(rkvc_rans_enc_sym *s,
                             uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint32_t scale = 1u << scale_bits;

    /* min(STATE_BITS, 31) = min(63, 31) = 31 */
    s->x_max_hi = freq << (31 - scale_bits);

    if (freq > 1) {
        uint32_t shift = 1;
        while (freq > (1u << shift))
            shift++;
        /* uint64_t state 倒数：32 位跳过 + 恢复 */
        uint64_t x0 = (uint64_t)(freq - 1);
        uint64_t x1 = 1ULL << (shift + 31);
        uint64_t t1 = x1 / freq;
        x0 += (x1 % freq) << 32;
        uint64_t t0 = x0 / freq;
        uint64_t rcp = t0 + (t1 << 32);
        s->freq_rcp    = (uint32_t)rcp;
        s->freq_rcp_hi = (uint32_t)(rcp >> 32);
        s->freq_rcp_shift = shift - 1;  /* 无额外移位（uint64_t state）*/
        s->bias = start;
    } else {
        s->freq_rcp    = 0xFFFFFFFFu;
        s->freq_rcp_hi = 0xFFFFFFFFu;
        s->freq_rcp_shift = 0;
        s->bias = start + scale - 1;
    }
    s->freq_cmpl = scale - freq;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  PMF 验证 + 分布描述构造                                            */
/* ════════════════════════════════════════════════════════════════════ */

static int init_dist_descs(rkvc_rans_dist *dists, size_t num_dists,
                            const int32_t *pmf_lengths,
                            const int32_t *pmf_offsets,
                            size_t table_size)
{
    size_t cursor = 0;
    for (size_t i = 0; i < num_dists; i++) {
        int32_t length = pmf_lengths[i];
        if (length <= 1 || table_size - cursor < (size_t)length)
            return RKVC_RANS_ERR_PMF;
        dists[i].value_offset = pmf_offsets[i];
        dists[i].bypass_sentinel = length - 1;
        dists[i].enc_sym_offset = cursor;
        cursor += (size_t)length;
    }
    if (cursor != table_size)
        return RKVC_RANS_ERR_PMF;
    return RKVC_RANS_OK;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  熵编解码器初始化（PMF → 编码符号表 + 解码 CDF 表）                 */
/* ════════════════════════════════════════════════════════════════════ */

int rkvc_rans_coder_init(rkvc_rans_coder *coder, rkvc_rans_variant variant,
                         const int32_t *pmf_lengths, size_t num_lengths,
                         const int32_t *pmf_offsets, size_t num_offsets,
                         const int32_t *pmf_table, size_t table_size,
                         int symbol_bits, int bypass_bits)
{
    if (!coder || !pmf_lengths || !pmf_offsets || !pmf_table)
        return RKVC_RANS_ERR_PARAMS;
    if (num_lengths != num_offsets)
        return RKVC_RANS_ERR_PMF;

    size_t max_scale = (variant == RKVC_RANS_BYTE)
                        ? RANS_BYTE_MAX_SCALE : RANS_64_MAX_SCALE;
    if (symbol_bits < 2 || (size_t)symbol_bits > max_scale)
        return RKVC_RANS_ERR_PARAMS;
    if (bypass_bits < 2 || (size_t)bypass_bits > max_scale)
        return RKVC_RANS_ERR_PARAMS;

    memset(coder, 0, sizeof(*coder));
    coder->variant = variant;
    coder->symbol_bits = symbol_bits;
    coder->bypass_bits = bypass_bits;
    coder->bypass_max_value = (1u << bypass_bits) - 1;

    /* 分布描述 */
    coder->dists = (rkvc_rans_dist *)calloc(num_lengths, sizeof(rkvc_rans_dist));
    if (!coder->dists)
        return RKVC_RANS_ERR_STATE;
    coder->num_dists = num_lengths;

    int rc = init_dist_descs(coder->dists, num_lengths,
                             pmf_lengths, pmf_offsets, table_size);
    if (rc != RKVC_RANS_OK)
        goto fail;

    /* ── 编码符号表（从 PMF 直接映射，无间隙）── */
    coder->enc_syms = (rkvc_rans_enc_sym *)calloc(table_size, sizeof(rkvc_rans_enc_sym));
    if (!coder->enc_syms)
        goto fail;
    coder->num_enc_syms = table_size;

    uint32_t max_freq = 1u << symbol_bits;
    {
        size_t tbl = 0;
        for (size_t d = 0; d < num_lengths; d++) {
            int32_t start = 0;
            for (int32_t i = 0; i <= coder->dists[d].bypass_sentinel; i++) {
                int32_t freq = pmf_table[tbl];
                if (!(freq > 0 && freq <= (int32_t)(max_freq - start)))
                    goto fail;
                if (variant == RKVC_RANS_BYTE)
                    enc_sym_init_byte(&coder->enc_syms[tbl],
                                      (uint32_t)start, (uint32_t)freq, symbol_bits);
                else
                    enc_sym_init_64(&coder->enc_syms[tbl],
                                    (uint32_t)start, (uint32_t)freq, symbol_bits);
                start += freq;
                tbl++;
            }
        }
    }

    /* ── 解码 CDF 表（含每分布一个尾项，有间隙）── */
    /* CDF 表大小 = table_size + num_lengths（每个分布多一个终止频率）*/
    coder->cdf_table_size = table_size + num_lengths;
    coder->cdf_table = (uint32_t *)calloc(coder->cdf_table_size, sizeof(uint32_t));
    if (!coder->cdf_table)
        goto fail;

    {
        size_t cursor = 0;
        for (size_t d = 0; d < num_lengths; d++) {
            rkvc_rans_dist *desc = &coder->dists[d];
            /* 更新解码 CDF 偏移（含累计间隙）*/
            desc->dec_cdf_offset = cursor + d;
            int32_t start = 0;
            for (int32_t i = 0; i <= desc->bypass_sentinel; i++, cursor++) {
                int32_t freq = pmf_table[cursor];
                if (!(freq > 0 && freq <= (int32_t)(max_freq - start)))
                    goto fail;
                coder->cdf_table[cursor + d] = (uint32_t)start;
                start += freq;
            }
            /* 尾项 = 总频率（= 1 << symbol_bits）*/
            coder->cdf_table[cursor + d] = (uint32_t)start;
        }
    }

    coder->initialized = 1;
    return RKVC_RANS_OK;

fail:
    rkvc_rans_coder_free(coder);
    return rc != RKVC_RANS_OK ? rc : RKVC_RANS_ERR_PMF;
}

void rkvc_rans_coder_free(rkvc_rans_coder *coder)
{
    if (!coder)
        return;
    free(coder->dists);
    free(coder->enc_syms);
    free(coder->cdf_table);
    coder->dists = NULL;
    coder->enc_syms = NULL;
    coder->cdf_table = NULL;
    coder->initialized = 0;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  内部：原始 rANS 编码操作                                            */
/* ════════════════════════════════════════════════════════════════════ */

/* ── 向后写入缓冲：确保有空间写入一个 unit ── */

static int enc_grow(rkvc_rans_enc_stream *s)
{
    if (s->oom || !s->buf)
        return -1;
    size_t new_cap = s->cap * 2;
    if (new_cap < s->cap + 65536)
        new_cap = s->cap + 65536;
    if (new_cap <= s->cap) {
        s->oom = 1;
        return -1;
    }
    uint8_t *new_buf = (uint8_t *)malloc(new_cap);
    if (!new_buf) {
        s->oom = 1;
        return -1;
    }
    size_t content = (size_t)(s->end - s->ptr);
    memcpy(new_buf + new_cap - content, s->ptr, content);
    free(s->buf);
    s->buf = new_buf;
    s->cap = new_cap;
    s->end = new_buf + new_cap;
    s->ptr = new_buf + new_cap - content;
    return 0;
}

static inline void enc_write_unit_byte(rkvc_rans_enc_stream *s, uint8_t val)
{
    if (s->oom || !s->buf || !s->ptr)
        return;
    if (s->ptr <= s->buf && enc_grow(s) != 0)
        return;
    if (s->ptr <= s->buf)
        return;
    *(--s->ptr) = val;
}

static inline void enc_write_unit_64(rkvc_rans_enc_stream *s, uint32_t val)
{
    if (s->oom || !s->buf || !s->ptr)
        return;
    if ((size_t)(s->ptr - s->buf) < 4 && enc_grow(s) != 0)
        return;
    if ((size_t)(s->ptr - s->buf) < 4)
        return;
    s->ptr -= 4;
    memcpy(s->ptr, &val, 4);  /* 原生字节序（aarch64 = LE）*/
}

/* ── Renormalize ── */

static inline void renorm_byte(rkvc_rans_enc_stream *s, uint32_t x_max)
{
    uint32_t x = (uint32_t)s->state;
    while (x >= x_max) {
        enc_write_unit_byte(s, (uint8_t)x);
        x >>= 8;
    }
    s->state = x;
}

static inline void renorm_64(rkvc_rans_enc_stream *s, uint64_t x_max)
{
    uint64_t x = s->state;
    /* MaxScaleBits(32) <= 32 → 最多一次 */
    while (x >= x_max) {
        enc_write_unit_64(s, (uint32_t)x);
        x >>= 32;
        break;
    }
    s->state = x;
}

/* ── Put（预计算符号）── */

static inline void put_sym_byte(rkvc_rans_enc_stream *s, const rkvc_rans_enc_sym *sym)
{
    /* x_max = m_x_max_hi（StateBits=31 不大于 31，无额外移位）*/
    renorm_byte(s, sym->x_max_hi);
    uint32_t x = (uint32_t)s->state;
    uint32_t q = (uint32_t)(((uint64_t)x * sym->freq_rcp) >> sym->freq_rcp_shift);
    x += q * sym->freq_cmpl + sym->bias;
    s->state = x;
}

static inline void put_sym_64(rkvc_rans_enc_stream *s, const rkvc_rans_enc_sym *sym)
{
    /* x_max = m_x_max_hi << (StateBits - 31 + 1) = m_x_max_hi << 33 */
    uint64_t x_max = ((uint64_t)sym->x_max_hi) << 33;
    renorm_64(s, x_max);
    uint64_t x = s->state;
    uint64_t rcp = ((uint64_t)sym->freq_rcp_hi << 32) | sym->freq_rcp;
    uint64_t q = mul64hi(x, rcp) >> sym->freq_rcp_shift;
    x += q * sym->freq_cmpl + sym->bias;
    s->state = x;
}

/* ── Put（原始：start, freq, scale_bits）── 用于 bypass 编码 ── */

static inline void put_raw_byte(rkvc_rans_enc_stream *s,
                                uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint32_t x_max = freq << (RANS_BYTE_STATE_BITS - scale_bits);
    renorm_byte(s, x_max);
    uint32_t x = (uint32_t)s->state;
    x = ((x / freq) << scale_bits) + start + (x % freq);
    s->state = x;
}

static inline void put_raw_64(rkvc_rans_enc_stream *s,
                              uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint64_t x_max = (uint64_t)freq << (RANS_64_STATE_BITS - scale_bits);
    renorm_64(s, x_max);
    uint64_t x = s->state;
    x = ((x / freq) << scale_bits) + start + (x % freq);
    s->state = x;
}

/* ── Flush ── */

static inline void flush_byte(rkvc_rans_enc_stream *s)
{
    uint32_t x = (uint32_t)s->state;
    /* sizeof(uint32_t)/sizeof(uint8_t) - 1 = 3 */
    for (int i = 3; i > 0; i--)
        enc_write_unit_byte(s, (uint8_t)(x >> (i * 8)));
    enc_write_unit_byte(s, (uint8_t)x);
}

static inline void flush_64(rkvc_rans_enc_stream *s)
{
    uint64_t x = s->state;
    /* sizeof(uint64_t)/sizeof(uint32_t) - 1 = 1 */
    enc_write_unit_64(s, (uint32_t)(x >> 32));
    enc_write_unit_64(s, (uint32_t)x);
}

/* ════════════════════════════════════════════════════════════════════ */
/*  内部：原始 rANS 解码操作                                            */
/* ════════════════════════════════════════════════════════════════════ */

static inline int dec_read_byte(rkvc_rans_dec_stream *s, uint8_t *out)
{
    if (s->ptr >= s->end)
        return -1;
    *out = *s->ptr++;
    return 0;
}

static inline int dec_read_64(rkvc_rans_dec_stream *s, uint32_t *out)
{
    if ((size_t)(s->end - s->ptr) < 4)
        return -1;
    memcpy(out, s->ptr, 4);
    s->ptr += 4;
    return 0;
}

static inline int dec_init_byte(rkvc_rans_dec_stream *s)
{
    uint8_t u;
    if (dec_read_byte(s, &u))
        return -1;
    uint32_t x = u;
    for (int i = 1; i < RANS_BYTE_UNITS_PER_STATE; i++) {
        if (dec_read_byte(s, &u))
            return -1;
        x += (uint32_t)u << (i * 8);
    }
    if (x < RANS_BYTE_LOWER_BOUND)
        return -1;
    s->state = x;
    return 0;
}

static inline int dec_init_64(rkvc_rans_dec_stream *s)
{
    uint32_t u;
    if (dec_read_64(s, &u))
        return -1;
    uint64_t x = u;
    for (int i = 1; i < RANS_64_UNITS_PER_STATE; i++) {
        if (dec_read_64(s, &u))
            return -1;
        x += (uint64_t)u << (i * 32);
    }
    if (x < RANS_64_LOWER_BOUND)
        return -1;
    s->state = x;
    return 0;
}

static inline uint32_t dec_get_byte(rkvc_rans_dec_stream *s, uint32_t scale_bits)
{
    return (uint32_t)s->state & ((1u << scale_bits) - 1);
}

static inline uint32_t dec_get_64(rkvc_rans_dec_stream *s, uint32_t scale_bits)
{
    return (uint32_t)s->state & ((1u << scale_bits) - 1);
}

static inline int dec_advance_byte(rkvc_rans_dec_stream *s,
                                   uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint32_t scale = 1u << scale_bits;
    uint32_t x = (uint32_t)s->state;
    uint32_t value = x & (scale - 1);
    x = freq * (x >> scale_bits) + value - start;
    while (x < RANS_BYTE_LOWER_BOUND) {
        uint8_t u;
        if (dec_read_byte(s, &u))
            return -1;
        x = (x << 8) + u;
    }
    s->state = x;
    return 0;
}

static inline int dec_advance_64(rkvc_rans_dec_stream *s,
                                 uint32_t start, uint32_t freq, uint32_t scale_bits)
{
    uint32_t scale = 1u << scale_bits;
    uint64_t x = s->state;
    uint32_t value = (uint32_t)(x & (scale - 1));
    x = (uint64_t)freq * (x >> scale_bits) + value - start;
    /* MaxScaleBits(32) <= 32 → 最多一次 */
    while (x < RANS_64_LOWER_BOUND) {
        uint32_t u;
        if (dec_read_64(s, &u))
            return -1;
        x = (x << 32) + u;
        break;
    }
    s->state = x;
    return 0;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  Bypass 编解码                                                      */
/* ════════════════════════════════════════════════════════════════════ */

/* 前向声明：变体分派辅助 */
static inline void put_raw_byte_or_64(rkvc_rans_enc_stream *s,
                                      rkvc_rans_variant variant,
                                      uint32_t start, uint32_t freq, uint32_t sb);
static inline int dec_advance_byte_or_64(rkvc_rans_dec_stream *s,
                                         rkvc_rans_variant variant,
                                         uint32_t start, uint32_t freq, uint32_t sb);

static void enc_bypass(rkvc_rans_enc_stream *s, const rkvc_rans_coder *c,
                       uint32_t bypass_value)
{
    /* s_MaxBypassParts = sizeof(freq_t)*8/2 = 16 */
    uint32_t buf[16];
    int n = 0;
    uint32_t v = bypass_value;
    while (v != 0) {
        buf[n++] = v & c->bypass_max_value;
        v >>= c->bypass_bits;
    }
    /* 保留总部分数（n 会被逆序写入循环消耗）*/
    int total_parts = n;
    /* 逆序写入部分 */
    while (n > 0)
        put_raw_byte_or_64(s, c->variant, buf[--n], 1, c->bypass_bits);

    /* 前缀计数 */
    int prefix = 0;
    uint32_t count = (uint32_t)total_parts;
    while (count >= c->bypass_max_value) {
        count -= c->bypass_max_value;
        prefix++;
    }
    put_raw_byte_or_64(s, c->variant, count, 1, c->bypass_bits);
    while (prefix > 0) {
        put_raw_byte_or_64(s, c->variant, c->bypass_max_value, 1, c->bypass_bits);
        prefix--;
    }
}

/* ── bypass 原始写入分派 ── */

static inline void put_raw_byte_or_64(rkvc_rans_enc_stream *s,
                                      rkvc_rans_variant variant,
                                      uint32_t start, uint32_t freq, uint32_t sb)
{
    if (variant == RKVC_RANS_BYTE)
        put_raw_byte(s, start, freq, sb);
    else
        put_raw_64(s, start, freq, sb);
}

/* ── bypass 解码 ── */

static int dec_bypass(rkvc_rans_dec_stream *s, const rkvc_rans_coder *c,
                      uint32_t *out_value)
{
    uint32_t value;
    uint32_t bypass_count;

    /* Step 1: 读取 bypass count */
    value = (c->variant == RKVC_RANS_BYTE)
        ? dec_get_byte(s, c->bypass_bits) : dec_get_64(s, c->bypass_bits);
    if (dec_advance_byte_or_64(s, c->variant, value, 1, c->bypass_bits))
        return -1;
    bypass_count = value;
    while (value == c->bypass_max_value) {
        value = (c->variant == RKVC_RANS_BYTE)
            ? dec_get_byte(s, c->bypass_bits) : dec_get_64(s, c->bypass_bits);
        if (dec_advance_byte_or_64(s, c->variant, value, 1, c->bypass_bits))
            return -1;
        bypass_count += value;
        if (bypass_count > 32)  /* sizeof(freq_t)*CHAR_BIT */
            return -1;
    }

    /* Step 2: 读取 bypass value */
    uint32_t encoded = 0;
    uint32_t total_bits = bypass_count * c->bypass_bits;
    for (uint32_t shift = 0; shift < total_bits; shift += c->bypass_bits) {
        value = (c->variant == RKVC_RANS_BYTE)
            ? dec_get_byte(s, c->bypass_bits) : dec_get_64(s, c->bypass_bits);
        if (dec_advance_byte_or_64(s, c->variant, value, 1, c->bypass_bits))
            return -1;
        encoded |= value << shift;
    }
    *out_value = encoded;
    return 0;
}

static inline int dec_advance_byte_or_64(rkvc_rans_dec_stream *s,
                                         rkvc_rans_variant variant,
                                         uint32_t start, uint32_t freq, uint32_t sb)
{
    if (variant == RKVC_RANS_BYTE)
        return dec_advance_byte(s, start, freq, sb);
    else
        return dec_advance_64(s, start, freq, sb);
}

/* ════════════════════════════════════════════════════════════════════ */
/*  流式编码器                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void rkvc_rans_enc_stream_init(rkvc_rans_enc_stream *s,
                               rkvc_rans_variant variant,
                               size_t initial_capacity)
{
    memset(s, 0, sizeof(*s));
    s->variant = variant;
    if (initial_capacity < 512)
        initial_capacity = 512;
    s->buf = (uint8_t *)malloc(initial_capacity);
    if (!s->buf) {
        s->oom = 1;
        s->cap = 0;
        s->end = NULL;
        s->ptr = NULL;
        s->state = (variant == RKVC_RANS_BYTE)
            ? RANS_BYTE_LOWER_BOUND : RANS_64_LOWER_BOUND;
        return;
    }
    s->cap = initial_capacity;
    s->end = s->buf + initial_capacity;
    s->ptr = s->end;  /* 从末尾向前写 */
    s->state = (variant == RKVC_RANS_BYTE)
        ? RANS_BYTE_LOWER_BOUND : RANS_64_LOWER_BOUND;
}

int rkvc_rans_enc_stream_encode(rkvc_rans_enc_stream *s,
                                const rkvc_rans_coder *coder,
                                const int32_t *indices,
                                const int32_t *values, size_t count)
{
    if (!s || !coder || !indices || !values)
        return RKVC_RANS_ERR_PARAMS;
    if (!coder->initialized)
        return RKVC_RANS_ERR_STATE;
    if (s->flushed || s->oom || !s->buf)
        return RKVC_RANS_ERR_STATE;

    /* 数据逆序编码（与 C++ 一致）*/
    for (size_t i = count; i > 0; i--) {
        int32_t index = indices[i - 1];
        if (index < 0)
            continue;  /* 跳过：解码时返回 0 */

        /* clamp 到有效范围 */
        if ((size_t)index >= coder->num_dists)
            index = (int32_t)coder->num_dists - 1;
        const rkvc_rans_dist *desc = &coder->dists[index];

        int32_t value = values[i - 1] + desc->value_offset;
        if (value < 0 || value >= desc->bypass_sentinel) {
            /* 超出符号范围 → bypass 编码 */
            uint32_t bv;
            if (value < 0)
                bv = 2u * (uint32_t)(-value) - 1;
            else
                bv = 2u * (uint32_t)(value - desc->bypass_sentinel);
            enc_bypass(s, coder, bv);
            value = desc->bypass_sentinel;
        }

        size_t sym = desc->enc_sym_offset + (size_t)value;
        if (s->variant == RKVC_RANS_BYTE)
            put_sym_byte(s, &coder->enc_syms[sym]);
        else
            put_sym_64(s, &coder->enc_syms[sym]);
        if (s->oom)
            return RKVC_RANS_ERR_STATE;
    }
    return RKVC_RANS_OK;
}

const uint8_t *rkvc_rans_enc_stream_flush(rkvc_rans_enc_stream *s,
                                          size_t *out_size)
{
    if (!s || s->flushed || s->oom || !s->buf) {
        if (out_size) *out_size = 0;
        return NULL;
    }
    if (s->variant == RKVC_RANS_BYTE)
        flush_byte(s);
    else
        flush_64(s);
    if (s->oom) {
        if (out_size) *out_size = 0;
        return NULL;
    }

    s->flushed = 1;
    if (out_size)
        *out_size = (size_t)(s->end - s->ptr);
    return s->ptr;
}

void rkvc_rans_enc_stream_reset(rkvc_rans_enc_stream *s)
{
    if (!s)
        return;
    s->ptr = s->end;
    s->flushed = 0;
    s->state = (s->variant == RKVC_RANS_BYTE)
        ? RANS_BYTE_LOWER_BOUND : RANS_64_LOWER_BOUND;
}

void rkvc_rans_enc_stream_free(rkvc_rans_enc_stream *s)
{
    if (!s)
        return;
    free(s->buf);
    s->buf = NULL;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  流式解码器                                                          */
/* ════════════════════════════════════════════════════════════════════ */

void rkvc_rans_dec_stream_init(rkvc_rans_dec_stream *s,
                               rkvc_rans_variant variant)
{
    memset(s, 0, sizeof(*s));
    s->variant = variant;
}

int rkvc_rans_dec_stream_open(rkvc_rans_dec_stream *s,
                              const uint8_t *data, size_t size)
{
    if (!s || !data)
        return RKVC_RANS_ERR_PARAMS;

    size_t unit_sz = (s->variant == RKVC_RANS_BYTE) ? 1 : 4;
    if (size % unit_sz != 0)
        return RKVC_RANS_ERR_STREAM;

    s->ptr = data;
    s->end = data + size;
    s->opened = 0;

    int rc = (s->variant == RKVC_RANS_BYTE) ? dec_init_byte(s) : dec_init_64(s);
    if (rc)
        return RKVC_RANS_ERR_STREAM;

    s->opened = 1;
    return RKVC_RANS_OK;
}

int rkvc_rans_dec_stream_decode(rkvc_rans_dec_stream *s,
                                const rkvc_rans_coder *coder,
                                int32_t *values,
                                const int32_t *indices, size_t count)
{
    if (!s || !coder || !values || !indices)
        return RKVC_RANS_ERR_PARAMS;
    if (!coder->initialized || !s->opened)
        return RKVC_RANS_ERR_STATE;

    for (size_t i = 0; i < count; i++) {
        int32_t index = indices[i];
        if (index < 0) {
            values[i] = 0;
            continue;
        }
        if ((size_t)index >= coder->num_dists)
            index = (int32_t)coder->num_dists - 1;
        const rkvc_rans_dist *desc = &coder->dists[index];

        uint32_t cum_freq = (s->variant == RKVC_RANS_BYTE)
            ? dec_get_byte(s, coder->symbol_bits)
            : dec_get_64(s, coder->symbol_bits);

        /* CDF 二分查找 */
        const uint32_t *base = coder->cdf_table + desc->dec_cdf_offset;
        int32_t lo = 0;
        int32_t hi = desc->bypass_sentinel + 1;  /* upper_bound 范围 */
        while (lo < hi) {
            int32_t mid = lo + (hi - lo) / 2;
            if (base[mid + 1] <= cum_freq)
                lo = mid + 1;
            else
                hi = mid;
        }
        int32_t symbol = lo;
        uint32_t start = base[symbol];
        uint32_t freq = base[symbol + 1] - base[symbol];

        if (dec_advance_byte_or_64(s, s->variant, start, freq, coder->symbol_bits))
            return RKVC_RANS_ERR_STREAM;

        if (symbol == desc->bypass_sentinel) {
            uint32_t bv;
            if (dec_bypass(s, coder, &bv))
                return RKVC_RANS_ERR_STREAM;
            if (bv & 1)
                symbol = -(int32_t)(bv >> 1) - 1;
            else
                symbol = (int32_t)(bv >> 1) + desc->bypass_sentinel;
        }
        values[i] = symbol - desc->value_offset;
    }
    return RKVC_RANS_OK;
}

int rkvc_rans_dec_stream_check_eof(const rkvc_rans_dec_stream *s)
{
    if (!s || !s->opened)
        return 0;
    if (s->ptr != s->end)
        return 0;
    if (s->variant == RKVC_RANS_BYTE)
        return (uint32_t)s->state == RANS_BYTE_LOWER_BOUND;
    else
        return s->state == RANS_64_LOWER_BOUND;
}

void rkvc_rans_dec_stream_close(rkvc_rans_dec_stream *s)
{
    if (!s)
        return;
    s->ptr = NULL;
    s->end = NULL;
    s->opened = 0;
    s->state = 0;
}

/* ════════════════════════════════════════════════════════════════════ */
/*  一次性 API                                                          */
/* ════════════════════════════════════════════════════════════════════ */

int rkvc_rans_encode(const rkvc_rans_coder *coder,
                     const int32_t *indices,
                     const int32_t *values, size_t count,
                     uint8_t **out, size_t *out_sz)
{
    if (!out || !out_sz)
        return RKVC_RANS_ERR_PARAMS;
    *out = NULL;
    *out_sz = 0;

    rkvc_rans_enc_stream s;
    rkvc_rans_enc_stream_init(&s, coder->variant, 65536);
    int rc = rkvc_rans_enc_stream_encode(&s, coder, indices, values, count);
    if (rc != RKVC_RANS_OK) {
        rkvc_rans_enc_stream_free(&s);
        return rc;
    }
    size_t sz;
    const uint8_t *data = rkvc_rans_enc_stream_flush(&s, &sz);
    if (!data) {
        rkvc_rans_enc_stream_free(&s);
        return RKVC_RANS_ERR_STATE;
    }
    *out = (uint8_t *)malloc(sz);
    if (!*out) {
        rkvc_rans_enc_stream_free(&s);
        return RKVC_RANS_ERR_STATE;
    }
    memcpy(*out, data, sz);
    *out_sz = sz;
    rkvc_rans_enc_stream_free(&s);
    return RKVC_RANS_OK;
}

int rkvc_rans_decode(const rkvc_rans_coder *coder,
                     int32_t *values,
                     const int32_t *indices, size_t count,
                     const uint8_t *data, size_t size)
{
    rkvc_rans_dec_stream s;
    rkvc_rans_dec_stream_init(&s, coder->variant);
    int rc = rkvc_rans_dec_stream_open(&s, data, size);
    if (rc != RKVC_RANS_OK)
        return rc;
    rc = rkvc_rans_dec_stream_decode(&s, coder, values, indices, count);
    if (rc != RKVC_RANS_OK)
        return rc;
    if (!rkvc_rans_dec_stream_check_eof(&s))
        return RKVC_RANS_ERR_STREAM;
    return RKVC_RANS_OK;
}
