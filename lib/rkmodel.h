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

/** @brief 已解析头部（内存形态；字符串已 NUL 截断）。 */
typedef struct rkvc_rkmodel {
    rkvc_model_info info;                       /**< 公共摘要 */
    uint32_t        payload_count;              /**< 载荷表项数 */
    rkmodel_payload_entry payloads[RKMODEL_MAX_PAYLOADS]; /**< 载荷表 */
    uint32_t        min_runtime_abi;            /**< 0 = 未声明 */
    char            path[1024];                 /**< 来源文件（载荷按需装载） */
} rkvc_rkmodel;

/**
 * @brief 读取并校验 .rkmodel 头部、TLV、载荷表和文件边界。
 *
 * 只读有界头部，不装载载荷。所有长度读取前检查上界；未知 TLV 跳过；
 * 字符串按字段容量截断；载荷范围必须完全位于文件数据区且互不重叠。
 *
 * @return RKVC_STATUS_OK / RKVC_STATUS_INVALID（格式违例） /
 *         RKVC_STATUS_IO。
 */
rkvc_status rkvc_rkmodel_open(const char *path, rkvc_rkmodel *out,
                              char *errbuf, size_t errcap);

/**
 * @brief 把指定载荷完整读入自分配缓冲（*buf 归调用方释放）。
 */
rkvc_status rkvc_rkmodel_load_payload(const rkvc_rkmodel *m, uint32_t kind,
                                      void **buf, size_t *size);

#endif /* RKVC_RKMODEL_H */
