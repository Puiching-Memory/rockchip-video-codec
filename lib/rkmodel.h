/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkmodel.h
 * @brief .rkmodel 容器读取器与注册表扫描（内部接口）。
 */

#ifndef RKVC_RKMODEL_H
#define RKVC_RKMODEL_H

#include "rkmodel_layout.h"
#include "rkvc/model.h"

#include <stdio.h>

/** @brief 已解析头部（内存形态；字符串已 NUL 截断）。 */
typedef struct rkvc_rkmodel {
    rkvc_model_info info;                       /**< 公共摘要 */
    uint32_t        payload_count;              /**< 载荷表项数 */
    rkmodel_payload_entry payloads[RKMODEL_MAX_PAYLOADS]; /**< 载荷表 */
    int             has_signature;              /**< 文件含签名尾 */
    char            key_slot[33];               /**< 可空 */
    uint32_t        min_runtime_abi;            /**< 0 = 未声明 */
    char            path[1024];                 /**< 来源文件（载荷按需装载） */
} rkvc_rkmodel;

/**
 * @brief 签名验证回调：对 signed_bytes 区间字节验证 Ed25519 签名。
 *
 * @param key_id   签名尾中的 key 标识。
 * @param sig      64 字节签名。
 * @param bytes    被签名字节流（固定头+TLV+载荷表，连续）。
 * @param len      字节数。
 * @param trust    出参：验证通过时的信任级别。
 * @param opaque   调用方上下文。
 * @return 0 验证通过；非 0 拒绝。
 */
typedef int (*rkvc_rkmodel_verify_fn)(const uint8_t key_id[16],
                                      const uint8_t sig[64],
                                      const uint8_t *bytes, size_t len,
                                      rkvc_model_trust *trust, void *opaque);

/**
 * @brief 读取并校验 .rkmodel 头部（TLV + 载荷表 + 可选签名）。
 *
 * 只读有界头部（固定头 + header_len 字节 + 载荷表 + 签名尾），不装载
 * 载荷。所有长度读取前检查上界；未知 TLV 跳过；字符串按字段容量截断。
 * 无验证回调时，已签名文件标 RKVC_MODEL_TRUST_UNTRUSTED。
 *
 * @return RKVC_STATUS_OK / RKVC_STATUS_INVALID（格式违例） /
 *         RKVC_STATUS_IO。
 */
rkvc_status rkvc_rkmodel_open(const char *path, rkvc_rkmodel *out,
                              rkvc_rkmodel_verify_fn verify, void *opaque,
                              char *errbuf, size_t errcap);

/**
 * @brief 校验指定载荷的 SHA-256 与表中摘要不一致时返回非 OK。
 *        kind 为 RKMODEL_PAYLOAD_*；找不到该载荷返回 RKVC_STATUS_NOT_FOUND。
 */
rkvc_status rkvc_rkmodel_check_payload(FILE *f, const rkvc_rkmodel *m,
                                       uint32_t kind);

/**
 * @brief 校验并把指定载荷完整读入自分配缓冲（*buf 归调用方释放）。
 *
 * 先按 check_payload 语义做 SHA-256 全量校验，再一次性读出载荷字节；
 * 摘要不符返回 RKVC_STATUS_INTEGRITY，不返回部分数据。
 */
rkvc_status rkvc_rkmodel_load_payload(const rkvc_rkmodel *m, uint32_t kind,
                                      void **buf, size_t *size);

/** 编译期 trust root 验证器；未启用 RKVC_ENABLE_MODEL_SIGN 时返回 NULL。 */
rkvc_rkmodel_verify_fn rkvc_model_trust_verifier(void);

/** 非 0 表示生产信任模式（unsigned 模型视为 untrusted）。 */
int rkvc_model_trust_production_mode(void);

#endif /* RKVC_RKMODEL_H */
