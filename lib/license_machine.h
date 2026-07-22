/**
 * @file license_machine.h
 * @brief 硬件指纹采集与机器码派生（lib/license.c 与 tools/rkvc_lic.c 共享）。
 *
 * 签发端（rkvc_lic）与校验端（librkvc）必须用完全相同的指纹算法，否则签发的
 * 机器码与本地校验时算出的机器码不一致，导致校验静默失败。两端链接同一份实现。
 *
 * 调用方须已调用 sodium_init()。
 */
#ifndef RKVC_LICENSE_MACHINE_H
#define RKVC_LICENSE_MACHINE_H

#include <stddef.h>

/** 机器码 hex 长度（64 字符 + NUL）；与公共 RKVC_MACHINE_ID_HEX_LEN 一致。 */
#define LIC_MACHINE_ID_HEX_LEN 65
#define LIC_FP_RAW_MAX         256
#define LIC_FP_PATH_MAX        320
#define LIC_FP_NOTE_MAX        160

/**
 * @brief 指纹采集详情（成功与失败均可填充 note_*，便于诊断）。
 *
 * 成功时 tag/path/raw/machine_id 有效；失败时 selected 字段为空串，
 * note_dt / note_otp / note_mac 说明每一级为何未采用。
 */
typedef struct lic_fp_info {
    char tag[32];                          /**< "dt-serial" | "otp" | "mac" */
    char path[LIC_FP_PATH_MAX];            /**< 实际读取路径或网卡名 */
    char raw[LIC_FP_RAW_MAX];              /**< 指纹原始值（文本） */
    char machine_id[LIC_MACHINE_ID_HEX_LEN];
    char note_dt[LIC_FP_NOTE_MAX];
    char note_otp[LIC_FP_NOTE_MAX];
    char note_mac[LIC_FP_NOTE_MAX];
} lic_fp_info;

/**
 * @brief 采集本机硬件指纹并派生机器码（含各级探测说明）。
 *
 * 指纹来源优先级：设备树序列号 → Rockchip OTP → 网卡 MAC。
 * 算法："<tag>:<raw>" 经 SHA-256，输出 64 字符十六进制（+ NUL）。
 *
 * @return 0 成功；-1 失败（无法采集任一指纹）。
 */
int lic_machine_id_collect(lic_fp_info *info);

/**
 * @brief 采集本机硬件指纹并派生机器码（仅输出 hex）。
 *
 * @param out_hex  输出缓冲，容量须 >= LIC_MACHINE_ID_HEX_LEN。
 * @param out_size out_hex 容量。
 * @return 0 成功；-1 失败（参数无效、缓冲过小或无法采集指纹）。
 */
int lic_machine_id_hex(char *out_hex, size_t out_size);

#endif /* RKVC_LICENSE_MACHINE_H */
