/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkmodel_layout.h
 * @brief .rkmodel v1 容器线格式（读写两侧单一来源）。
 *
 * 布局（全部小端序）：
 *   固定头（64 字节）
 *   TLV 头区（header_len 字节，有界 <= RKMODEL_MAX_HEADER）
 *   载荷表（payload_count x 24 字节）
 *   载荷数据（按表项 offset/length 寻址）
 *
 * 未知 TLV tag 必须可跳过；整数端序固定；所有长度读前检查上界。
 */

#ifndef RKMODEL_LAYOUT_H
#define RKMODEL_LAYOUT_H

#include <stdint.h>

#define RKMODEL_MAGIC           0x464d4b52u /* "RKMF" 小端 */
#define RKMODEL_VERSION         1u
#define RKMODEL_FIXED_SIZE      64u
#define RKMODEL_MAX_HEADER      (1u << 20)  /* TLV 区上界 1 MiB */
#define RKMODEL_MAX_PAYLOADS    16u
#define RKMODEL_MAX_SCAN_FILES  256u

/* TLV tags（字符串值均不带 NUL；未知 tag 跳过） */
#define RKMODEL_TAG_FAMILY      1u   /* 模型族，如 "sr"/"mlvc" */
#define RKMODEL_TAG_ROLE        2u   /* 角色，如 "encoder"/"decoder"/"upscale" */
#define RKMODEL_TAG_ID          3u   /* 稳定模型 ID */
#define RKMODEL_TAG_VERSION     4u   /* 模型版本串 */
#define RKMODEL_TAG_RKNN_TARGET 5u   /* RKNN 编译目标，如 "rk3588" */
#define RKMODEL_TAG_IO_CONTRACT 6u   /* I/O 契约描述 */
#define RKMODEL_TAG_QUANT       7u   /* 量化方法 */
#define RKMODEL_TAG_MIN_ABI     8u   /* 需要的最小运行时 ABI */

/* 载荷类型 */
#define RKMODEL_PAYLOAD_RKNN         1u
#define RKMODEL_PAYLOAD_PMF          2u
#define RKMODEL_PAYLOAD_QPPATCH      3u
#define RKMODEL_PAYLOAD_PMF_GAUSSIAN 4u
#define RKMODEL_PAYLOAD_PMF_BITEST   5u

#pragma pack(push, 1)
typedef struct rkmodel_fixed {
    uint32_t magic;
    uint32_t format_version;
    uint32_t header_len;     /* TLV 区字节数 */
    uint32_t payload_count;
    uint32_t payload_entry_size; /* 恒为 sizeof(rkmodel_payload_entry) */
    uint8_t  reserved[44];   /* 恒为 0 */
} rkmodel_fixed;             /* 64 字节 */

typedef struct rkmodel_payload_entry {
    uint32_t kind;
    uint32_t flags;
    uint64_t offset;         /* 相对文件起始 */
    uint64_t length;
} rkmodel_payload_entry;     /* 24 字节 */
#pragma pack(pop)

#endif /* RKMODEL_LAYOUT_H */
