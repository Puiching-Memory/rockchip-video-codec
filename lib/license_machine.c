/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file license_machine.c
 * @brief 硬件指纹采集与机器码派生（lib/license.c 与 tools/rkvc_lic.c 共享实现）。
 *
 * 这是机器码算法的唯一定义，确保签发端与校验端字节级一致。
 * 调用方须已调用 sodium_init()。
 */
#include "license_machine.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sodium.h>

static int read_file_text(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* 去尾部空白/NUL（设备树属性以 NUL 结尾） */
    while (n > 0 && (buf[n - 1] == '\0' || isspace((unsigned char)buf[n - 1])))
        buf[--n] = '\0';
    return n > 0 ? (int)n : -1;
}

static void note_open_fail(char *note, size_t note_sz, const char *path)
{
    int e = errno;
    if (e == ENOENT)
        snprintf(note, note_sz, "missing (%s)", path);
    else if (e == EACCES || e == EPERM)
        snprintf(note, note_sz, "permission denied (%s)", path);
    else
        snprintf(note, note_sz, "open failed (%s): %s", path, strerror(e));
}

static int try_dt_serial(char *raw, size_t raw_sz, char *path_out, size_t path_sz,
                         char *note, size_t note_sz)
{
    static const char *path = "/proc/device-tree/serial-number";
    snprintf(path_out, path_sz, "%s", path);
    errno = 0;
    int n = read_file_text(path, raw, raw_sz);
    if (n > 0) {
        snprintf(note, note_sz, "ok");
        return 0;
    }
    if (errno != 0)
        note_open_fail(note, note_sz, path);
    else
        snprintf(note, note_sz, "empty or unreadable (%s)", path);
    return -1;
}

static int try_otp(char *raw, size_t raw_sz, char *path_out, size_t path_sz,
                   char *note, size_t note_sz)
{
    static const char *path = "/sys/bus/nvmem/devices/rockchip-otp0/nvmem";
    snprintf(path_out, path_sz, "%s", path);
    errno = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        note_open_fail(note, note_sz, path);
        return -1;
    }
    uint8_t bin[32];
    size_t n = fread(bin, 1, sizeof(bin), f);
    int err = ferror(f);
    fclose(f);
    if (err || n == 0) {
        snprintf(note, note_sz, "empty or read error (%s)", path);
        return -1;
    }
    if (n * 2 >= raw_sz) {
        snprintf(note, note_sz, "buffer too small for %zu OTP bytes", n);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        snprintf(raw + i * 2, raw_sz - i * 2, "%02x", bin[i]);
    snprintf(note, note_sz, "ok (%zu bytes)", n);
    return 0;
}

static int try_mac(char *raw, size_t raw_sz, char *path_out, size_t path_sz,
                   char *note, size_t note_sz)
{
    DIR *dir = opendir("/sys/class/net");
    if (!dir) {
        note_open_fail(note, note_sz, "/sys/class/net");
        return -1;
    }
    int found = 0;
    struct dirent *ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        char vpath[320];
        snprintf(vpath, sizeof(vpath), "/sys/class/net/%s/virtual", ent->d_name);
        /* 跳过虚拟接口（lo/docker/br-…） */
        if (access(vpath, F_OK) == 0)
            continue;
        char apath[320];
        snprintf(apath, sizeof(apath), "/sys/class/net/%s/address", ent->d_name);
        if (read_file_text(apath, raw, raw_sz) > 0 &&
            strcmp(raw, "00:00:00:00:00:00") != 0) {
            snprintf(path_out, path_sz, "%s", apath);
            snprintf(note, note_sz, "ok (%.64s)", ent->d_name);
            found = 1;
        }
    }
    closedir(dir);
    if (!found) {
        snprintf(note, note_sz, "no usable physical NIC MAC");
        path_out[0] = '\0';
        return -1;
    }
    return 0;
}

/* 容器识别：标记文件或 PID 1 cgroup 特征。容器内 MAC 随实例重建而变，
 * 且同宿主机多容器可能同 MAC，不能作为 1机1码 的稳定指纹。 */
static int in_container(void)
{
    if (access("/.dockerenv", F_OK) == 0 ||
        access("/run/.containerenv", F_OK) == 0)
        return 1;
    char buf[2048];
    if (read_file_text("/proc/1/cgroup", buf, sizeof(buf)) > 0 &&
        (strstr(buf, "docker") || strstr(buf, "kubepods") ||
         strstr(buf, "containerd") || strstr(buf, "libpod") ||
         strstr(buf, "lxc")))
        return 1;
    return 0;
}

static int container_mac_allowed(void)
{
    const char *v = getenv("RKVC_LICENSE_ALLOW_CONTAINER_MAC");
    return v && v[0] == '1';
}

static void sha256_hex(const char *input, size_t input_len, char *out_hex)
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, (const unsigned char *)input,
                       (unsigned long)input_len);
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(out_hex + i * 2, 3, "%02x", digest[i]);
}

int lic_machine_id_collect(lic_fp_info *info)
{
    if (!info)
        return -1;
    memset(info, 0, sizeof(*info));

    char raw[LIC_FP_RAW_MAX];
    char path[LIC_FP_PATH_MAX];
    const char *tag = NULL;

    if (try_dt_serial(raw, sizeof(raw), path, sizeof(path),
                      info->note_dt, sizeof(info->note_dt)) == 0) {
        tag = "dt-serial";
    } else if (try_otp(raw, sizeof(raw), path, sizeof(path),
                       info->note_otp, sizeof(info->note_otp)) == 0) {
        tag = "otp";
    } else if (try_mac(raw, sizeof(raw), path, sizeof(path),
                       info->note_mac, sizeof(info->note_mac)) == 0) {
        tag = "mac";
    }

    /* 未选中的层级补全 note（成功短路时尚未探测的层级） */
    if (tag && strcmp(tag, "dt-serial") == 0) {
        snprintf(info->note_otp, sizeof(info->note_otp),
                 "skipped (higher priority selected)");
        snprintf(info->note_mac, sizeof(info->note_mac),
                 "skipped (higher priority selected)");
    } else if (tag && strcmp(tag, "otp") == 0) {
        snprintf(info->note_mac, sizeof(info->note_mac),
                 "skipped (higher priority selected)");
    } else if (!tag) {
        /* 三级均失败：补探测 otp/mac 若前面短路未跑到 */
        if (info->note_otp[0] == '\0')
            (void)try_otp(raw, sizeof(raw), path, sizeof(path),
                          info->note_otp, sizeof(info->note_otp));
        if (info->note_mac[0] == '\0')
            (void)try_mac(raw, sizeof(raw), path, sizeof(path),
                          info->note_mac, sizeof(info->note_mac));
        return -1;
    }

    /* 容器内拒绝 MAC 兜底，除非显式放行 */
    if (strcmp(tag, "mac") == 0 && in_container() && !container_mac_allowed()) {
        snprintf(info->note_mac, sizeof(info->note_mac),
                 "rejected: MAC fallback inside container (set "
                 "RKVC_LICENSE_ALLOW_CONTAINER_MAC=1 to override)");
        return -1;
    }

    /* 组合：tag + ':' + 原始值 → SHA-256 */
    char concat[512];
    int n = snprintf(concat, sizeof(concat), "%s:%s", tag, raw);
    if (n < 0 || (size_t)n >= sizeof(concat))
        return -1;

    snprintf(info->tag, sizeof(info->tag), "%s", tag);
    snprintf(info->path, sizeof(info->path), "%s", path);
    snprintf(info->raw, sizeof(info->raw), "%s", raw);
    sha256_hex(concat, (size_t)n, info->machine_id);
    return 0;
}

int lic_machine_id_hex(char *out_hex, size_t out_size)
{
    if (!out_hex || out_size < LIC_MACHINE_ID_HEX_LEN)
        return -1;

    lic_fp_info info;
    if (lic_machine_id_collect(&info) != 0)
        return -1;
    memcpy(out_hex, info.machine_id, LIC_MACHINE_ID_HEX_LEN);
    return 0;
}

int lic_machine_id_grouped(const char *hex64, char *out, size_t out_size)
{
    if (!hex64 || strlen(hex64) != 64 ||
        !out || out_size < LIC_MACHINE_ID_GROUPED_LEN)
        return -1;

    size_t o = 0;
    for (size_t i = 0; i < 64; i++) {
        if (i > 0 && (i % 4) == 0)
            out[o++] = '-';
        out[o++] = hex64[i];
    }
    out[o] = '\0';
    return 0;
}
