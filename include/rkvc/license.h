/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file license.h
 * @brief 1机1码授权（Ed25519 非对称签名）。
 *
 * 机器码由本机硬件指纹（设备树序列号 / OTP / MAC）经 SHA-256 派生；
 * 注册码由持有私钥的发码端对「机器码 + 产品」做 Ed25519 签名。
 * 库内嵌公钥本地校验，无需联网；私钥泄露前无法伪造注册码。
 *
 * 授权一经签发永久有效（无有效期字段，不做到期校验）。
 * 仅当 CMake 选项 `RKVC_ENABLE_LICENSE=ON` 时编译本模块。
 */
#ifndef RKVC_LICENSE_H
#define RKVC_LICENSE_H

#include <stdint.h>
#include <stddef.h>

#include "rkvc/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 机器码十六进制长度（SHA-256 = 32 字节 = 64 字符 + NUL）。 */
#define RKVC_MACHINE_ID_HEX_LEN 65

/** 产品标识。 */
#define RKVC_PRODUCT_RKVC 1u

/** 授权文件二进制大小（base64 注册码解码后须恰好等于此值）。 */
#define RKVC_LICENSE_BLOB_SIZE 104u

/**
 * @brief 许可证解析/校验结果详情。
 */
typedef struct {
    int      valid;            /**< 1 = 通过全部校验 */
    uint32_t product_id;       /**< 产品标识 */
    char     machine_id[RKVC_MACHINE_ID_HEX_LEN]; /**< 绑定的机器码（hex） */
    char     local_machine_id[RKVC_MACHINE_ID_HEX_LEN]; /**< 本机实际机器码（hex） */
    int      machine_matches;  /**< 1 = 机器码匹配本机 */
    int      signature_valid;  /**< 1 = Ed25519 签名有效 */
} rkvc_license_info;

/**
 * @brief 计算本机机器码。
 *
 * 指纹来源优先级：设备树序列号 → Rockchip OTP → 网卡 MAC。
 * 取首个可用项前缀标签后做 SHA-256，输出 64 字符十六进制。
 *
 * @param out_hex  输出缓冲，容量须 ≥ `RKVC_MACHINE_ID_HEX_LEN`。
 * @param out_size `out_hex` 容量。
 * @return `RKVC_OK`；`RKVC_ERR_INVALID`（缓冲过小）；`RKVC_ERR_HW`（无法采集指纹）。
 */
rkvc_err rkvc_machine_id(char *out_hex, size_t out_size);

/**
 * @brief 从文件读取并校验注册码。
 *
 * 文件内容为 base64 注册码（可含换行/空白）。依次校验：
 * 签名有效性 → 机器码是否匹配本机。
 *
 * @param path  授权文件路径。
 * @param info  非 NULL 时回填解析详情（无论成功失败）。
 * @return `RKVC_OK`；`RKVC_ERR_LICENSE`（任一校验失败）；`RKVC_ERR_IO`（读取失败）。
 */
rkvc_err rkvc_license_verify_file(const char *path, rkvc_license_info *info);

/**
 * @brief 校验已解码的许可证二进制（base64 解码后的原始 blob）。
 *
 * @param blob  许可证二进制，长度须 = `RKVC_LICENSE_BLOB_SIZE`。
 * @param len   `blob` 长度。
 * @param info  非 NULL 时回填解析详情。
 * @return `RKVC_OK`；`RKVC_ERR_LICENSE`；`RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_license_verify_blob(const uint8_t *blob, size_t len,
                                  rkvc_license_info *info);

/**
 * @brief 默认授权文件查找顺序：
 *   1. 环境变量 `RKVC_LICENSE_FILE` 指定的路径
 *   2. `~/.config/rkvc/license.lic`
 *
 * @param out_path 输出缓冲。
 * @param out_size `out_path` 容量。
 * @return `RKVC_OK`；`RKVC_ERR_NOT_FOUND`（无可用路径）；`RKVC_ERR_INVALID`。
 */
rkvc_err rkvc_license_default_path(char *out_path, size_t out_size);

/**
 * @brief 用默认路径校验授权（便捷封装）。
 * @param info 非 NULL 时回填详情。
 * @return 同 `rkvc_license_verify_file()`；无默认路径或文件不可读时返回 `RKVC_ERR_UNLICENSED`。
 */
rkvc_err rkvc_license_check(rkvc_license_info *info);

#ifdef __cplusplus
}
#endif

#endif /* RKVC_LICENSE_H */
