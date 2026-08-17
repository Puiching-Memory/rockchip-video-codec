/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file init.c
 * @brief 全局初始化、版本、能力查询。
 */

#include "internal.h"
#include "board.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

/*
 * 版本号统一由 CMake 的 project(VERSION) 生成，通过 target_compile_definitions
 * 注入为 RKVC_VERSION_STR / RKVC_VERSION_NUM。脱离 CMake 构建将直接编译失败，
 * 强制所有编译路径都走 CMake，避免版本号出现第二个来源。
 */

static pthread_mutex_t s_init_mutex = PTHREAD_MUTEX_INITIALIZER;
static int s_initialized = 0;

static rkvc_err rkvc_init_impl(void)
{
#ifdef RKVC_ENABLE_LICENSE
    rkvc_license_info lic;
    rkvc_err e = rkvc_license_check(&lic);
    if (e != RKVC_OK) {
        RKVC_LOG("license check failed (%s), refusing to initialize",
                 rkvc_err_str(e));
        return e;
    }
    RKVC_LOG("license OK (product=%u)", lic.product_id);
#endif

    rkvc_ffmpeg_utils_init();

    /* FFmpeg 全局初始化在较新版本中自动完成，此处确保网络子系统 */
#if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
    av_register_all();
    avformat_network_init();
#endif
    s_initialized = 1;
    RKVC_LOG("rkvc initialized");
    return RKVC_OK;
}

rkvc_err rkvc_init(void)
{
    pthread_mutex_lock(&s_init_mutex);
    if (!s_initialized) {
        rkvc_err e = rkvc_init_impl();
        if (e != RKVC_OK) {
            pthread_mutex_unlock(&s_init_mutex);
            return e;
        }
    }
    pthread_mutex_unlock(&s_init_mutex);
    return RKVC_OK;
}

void rkvc_deinit(void)
{
    /*
     * 注意: 不调用 avformat_network_deinit()。
     * 多个编解码器可能共享全局 FFmpeg 状态，
     * 强制反初始化会导致后续实例崩溃。
     */
    s_initialized = 0;
}

const char *rkvc_version(void)
{
    return RKVC_VERSION_STR;
}

uint32_t rkvc_version_number(void)
{
    return RKVC_VERSION_NUM;
}

const char *rkvc_err_str(rkvc_err err)
{
    switch (err) {
    case RKVC_OK:            return "success";
    case RKVC_ERR_NOMEM:     return "out of memory";
    case RKVC_ERR_INVALID:   return "invalid parameter";
    case RKVC_ERR_NOT_FOUND: return "codec or device not found";
    case RKVC_ERR_IO:        return "I/O error";
    case RKVC_ERR_HW:        return "hardware acceleration failed";
    case RKVC_ERR_EOF:       return "end of stream";
    case RKVC_ERR_AGAIN:     return "need more input / output full";
    case RKVC_ERR_MUX:       return "muxer error";
    case RKVC_ERR_INTERNAL:  return "internal error";
    case RKVC_ERR_PERMISSION: return "device permission denied";
    case RKVC_ERR_FORMAT:    return "input format mismatch";
    case RKVC_ERR_LICENSE:   return "license verification failed";
    case RKVC_ERR_UNLICENSED: return "no license found";
    }
    return "unknown error";
}

/* ── 能力查询 ──────────────────────────────────────────────────────── */

static void rkvc_dev_path(const char *path, char *buf, size_t size)
{
#ifdef RKVC_ENABLE_FAULT_INJECTION
    const char *root = getenv("RKVC_TEST_DEV_ROOT");
    if (root && root[0] != '\0') {
        snprintf(buf, size, "%s%s", root, path);
        return;
    }
#endif
    snprintf(buf, size, "%s", path);
}

static int rkvc_test_dev_path_denied(const char *path)
{
#ifdef RKVC_ENABLE_FAULT_INJECTION
    const char *deny = getenv("RKVC_TEST_DENY_DEV_PATH");
    if (deny && strcmp(deny, path) == 0)
        return 1;
#endif
    (void)path;
    return 0;
}

static int rkvc_dev_access(const char *path, int mode)
{
    if (rkvc_test_dev_path_denied(path)) {
        errno = EACCES;
        return -1;
    }

    char resolved[PATH_MAX];
    rkvc_dev_path(path, resolved, sizeof(resolved));
    return access(resolved, mode);
}

static int any_path_accessible(const char *const *paths, int mode)
{
    for (int i = 0; paths[i]; i++) {
        if (rkvc_dev_access(paths[i], mode) == 0)
            return 1;
    }
    return 0;
}

static int mpp_codec_device_accessible(void)
{
    static const char *const paths[] = {
        "/dev/mpp_service",
        "/dev/mpp-service",
        "/dev/rkvenc",
        "/dev/rkvdec",
        "/dev/h265e",
        "/dev/hevc_service",
        "/dev/hevc-service",
        "/dev/vpu_service",
        "/dev/vpu-service",
        NULL
    };

    return any_path_accessible(paths, R_OK | W_OK);
}

static int path_openable(const char *path, int flags)
{
    if (rkvc_test_dev_path_denied(path)) {
        errno = EACCES;
        return 0;
    }

    char resolved[PATH_MAX];
    rkvc_dev_path(path, resolved, sizeof(resolved));

    int fd = open(resolved, flags | O_CLOEXEC);
    if (fd < 0)
        return 0;

    close(fd);
    return 1;
}

static int dma_heap_runtime_selected(void)
{
    /*
     * MPP marks DMA heap valid from the directory permission alone. If this is
     * true it prefers DMA heap over DRM/ION, so child heap permissions must be
     * checked before entering RKMPP.
     */
    return rkvc_dev_access("/dev/dma_heap", F_OK | R_OK) == 0;
}

int rkvc_npu_accessible(void)
{
    if (rkvc_dev_access("/sys/kernel/debug/rknpu/version", R_OK) == 0)
        return 1;

    DIR *dir = opendir("/dev/dri/by-path");
    if (!dir)
        return 0;

    int found = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (!strstr(ent->d_name, "npu-render"))
            continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/dev/dri/by-path/%s", ent->d_name);
        if (rkvc_dev_access(path, R_OK | W_OK) == 0) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
}

static int drm_allocator_accessible(void)
{
    static const char *const paths[] = {
        "/dev/dri/renderD128",
        "/dev/dri/renderD129",
        "/dev/dri/card0",
        "/dev/dri/card1",
        NULL
    };

    return any_path_accessible(paths, R_OK | W_OK);
}

static int ion_allocator_accessible(void)
{
    static const char *const paths[] = {
        "/dev/ion",
        NULL
    };

    return any_path_accessible(paths, R_OK | W_OK);
}

static int mpp_default_dma_heap_accessible(void)
{
    /*
     * rockchip-mpp opens type 0 as /dev/dma_heap/system-uncached. If that
     * fails, it only remaps type 0 by flipping cacheable/dma32 flags, so cma*
     * heaps are not valid fallbacks for the default encoder/decoder path.
     */
    static const char *const names[] = {
        "system-uncached",
        "system",
        "system-uncached-dma32",
        "system-dma32",
        NULL
    };

    for (int i = 0; names[i]; i++) {
        char path[PATH_MAX];

        int written = snprintf(path, sizeof(path), "/dev/dma_heap/%s",
                               names[i]);
        if (written < 0 || (size_t)written >= sizeof(path))
            continue;

        if (path_openable(path, O_RDONLY))
            return 1;
    }

    return 0;
}

rkvc_err rkvc_check_hw_permissions(void)
{
    if (!mpp_codec_device_accessible()) {
        RKVC_LOG("MPP codec device permission denied: /dev/mpp_service or legacy codec nodes");
        return RKVC_ERR_PERMISSION;
    }

    if (dma_heap_runtime_selected()) {
        if (!mpp_default_dma_heap_accessible()) {
            RKVC_LOG("MPP default DMA heap permission denied: /dev/dma_heap/system-uncached");
            return RKVC_ERR_PERMISSION;
        }
        return RKVC_OK;
    }

    if (!drm_allocator_accessible() && !ion_allocator_accessible()) {
        RKVC_LOG("DMA-BUF allocator permission denied: /dev/dma_heap/* or /dev/dri/*");
        return RKVC_ERR_PERMISSION;
    }

    return RKVC_OK;
}

rkvc_err rkvc_query_caps(rkvc_caps *caps)
{
    if (!caps)
        return RKVC_ERR_INVALID;

    memset(caps, 0, sizeof(*caps));

    /*
     * 板卡 profile 是所有板级常量的唯一来源（lib/board.c）。
     * 硬件编解码能力 = 运行时探测（ffmpeg 注册 + 设备权限）∩ 板卡 VPU 硬件支持；
     * 软件编码器（SVT-AV1）与板卡无关，不做板卡门控。
     */
    const rkvc_board_profile *bp = rkvc_board_profile_active();
    caps->board = bp->id;

    rkvc_err hw_perm = rkvc_check_hw_permissions();
    int hw_usable = (hw_perm == RKVC_OK);

    caps->has_h264_enc = (avcodec_find_encoder_by_name("h264_rkmpp") != NULL
                          && hw_usable && bp->vpu_h264_enc_hw);
    caps->has_hevc_enc = (avcodec_find_encoder_by_name("hevc_rkmpp") != NULL
                          && hw_usable && bp->vpu_hevc_enc_hw);
    caps->has_av1_enc  = (avcodec_find_encoder_by_name("libsvtav1") != NULL);

    caps->has_h264_dec = (avcodec_find_decoder_by_name("h264_rkmpp") != NULL
                          && hw_usable && bp->vpu_h264_dec_hw);
    caps->has_hevc_dec = (avcodec_find_decoder_by_name("hevc_rkmpp") != NULL
                          && hw_usable && bp->vpu_hevc_dec_hw);
    caps->has_av1_dec  = (avcodec_find_decoder_by_name("av1_rkmpp") != NULL
                          && hw_usable && bp->vpu_av1_dec_hw);

    caps->has_dma_heap = mpp_default_dma_heap_accessible();
    caps->has_rga = bp->has_rga && (rkvc_dev_access("/dev/rga", R_OK | W_OK) == 0);
    caps->has_rknn = rkvc_rknn_sr_available() ? 1 : 0;

    /* max_width/height 取 VPU 硬件解码上限（与原 RK3588 7680×4320 语义一致） */
    caps->max_width  = bp->max_dec_w;
    caps->max_height = bp->max_dec_h;

    return RKVC_OK;
}
