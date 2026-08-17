/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file platform.c
 * @brief 运行时平台探测：所有硬件事实均来自系统信息，无预设板卡表。
 *
 * 探测来源：
 *  - SoC 名：/proc/device-tree/compatible 的 "rockchip,<soc>" 条目；
 *  - NPU：/sys/kernel/debug/rknpu/version 或 /dev/dri/by-path/*npu* 判存在，
 *    核心数取 rknpu debugfs `load` 的按核条目（CoreN:）计数；
 *  - RGA：/dev/rga 存在；
 *  - VPU 编解码能力：MPP `mpp_get_vcodec_type()`——内核驱动经
 *    mpp_service ioctl 上报的硬件能力位；内核不可用（如无设备权限的
 *    容器）时 MPP 内部按其 SoC 库兜底，rkvc 不再重复维护任何 SoC 表。
 */

#include "platform.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mpp_platform.h"
#include "mpp_dev_defs.h"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_done;
static rkvc_platform_info g_info;

/*
 * device-tree compatible 以 '\0' 分隔多个条目，板型条目在前、通用 SoC
 * 条目在后（如 "rockchip,rk3588-evb7-v11\0rockchip,rk3588"）。
 * 取最后一个 "rockchip,<soc>" 条目（截断至首个 '-'）即 SoC 名。
 */
static void probe_soc_name(char out[32])
{
    out[0] = '\0';
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    size_t off = 0;
    while (off < (size_t)n) {
        const char *entry = buf + off;
        size_t len = strlen(entry);
        const char *tok = strstr(entry, "rockchip,");
        if (tok) {
            tok += strlen("rockchip,");
            size_t k = 0;
            while (tok[k] && tok[k] != '-' && k + 1 < 32) {
                out[k] = (char)tolower((unsigned char)tok[k]);
                k++;
            }
            out[k] = '\0';
        }
        off += len + 1;   /* 跳过本条目及其结尾 '\0' */
    }
}

static int path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* NPU 存在性（存在即可，不要求访问权限）：rknpu debugfs 节点或 DRI NPU 节点 */
static int probe_npu_present(void)
{
    if (path_exists("/sys/kernel/debug/rknpu/version"))
        return 1;

    DIR *dir = opendir("/dev/dri/by-path");
    if (!dir)
        return 0;
    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strstr(ent->d_name, "npu")) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

/*
 * NPU 核心数：rknpu debugfs `load` 按核打印负载
 *（"NPU load:  Core0:  0%, Core1:  0%, ..."），数 CoreN: 条目个数。
 * debugfs 未挂载 / 不可读时返回 0 表示未知。
 */
static int probe_npu_cores(void)
{
    FILE *fp = fopen("/sys/kernel/debug/rknpu/load", "r");
    if (!fp)
        return 0;
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';

    int cores = 0;
    const char *p = buf;
    while ((p = strstr(p, "Core")) != NULL) {
        p += 4;
        if (!isdigit((unsigned char)*p))
            continue;
        while (isdigit((unsigned char)*p))
            p++;
        if (*p == ':')
            cores++;
    }
    return cores;
}

/*
 * VPU 能力位 → 编解码映射（MPP 引擎语义）：
 * 解码 VDPU1/VDPU2 仅 H.264，RKVDEC 覆盖 H.264/HEVC，AV1DEC 为 AV1；
 * 编码 VEPU1/VEPU2 仅 H.264，VEPU22 与 RKVENC 覆盖 H.264/HEVC。
 * Rockchip 无 AV1 硬件编码引擎（MPP 无对应 client 类型），
 * AV1 编码始终走软件（SVT-AV1），故无对应字段。
 */
static void probe_vpu(rkvc_platform_info *info)
{
    const rk_u32 v = mpp_get_vcodec_type();

    info->vpu_h264_dec_hw = !!(v & (HAVE_VDPU1 | HAVE_VDPU2 | HAVE_RKVDEC));
    info->vpu_hevc_dec_hw = !!(v & (HAVE_HEVC_DEC | HAVE_RKVDEC));
    info->vpu_av1_dec_hw  = !!(v & HAVE_AV1DEC);
    info->vpu_h264_enc_hw = !!(v & (HAVE_VEPU1 | HAVE_VEPU2 | HAVE_VEPU22 | HAVE_RKVENC));
    info->vpu_hevc_enc_hw = !!(v & (HAVE_VEPU22 | HAVE_RKVENC));
}

const rkvc_platform_info *rkvc_platform_probe(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_done) {
        probe_soc_name(g_info.soc);
        g_info.has_npu   = probe_npu_present();
        g_info.npu_cores = probe_npu_cores();
        g_info.has_rga   = path_exists("/dev/rga");
        probe_vpu(&g_info);
        g_done = 1;
    }
    pthread_mutex_unlock(&g_lock);
    return &g_info;
}
