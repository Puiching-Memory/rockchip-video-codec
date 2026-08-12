/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file board.c
 * @brief 板卡 profile 表与运行时探测。
 */

#include "board.h"

#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * ── 板卡 profile 表 ──────────────────────────────────────────────────
 * 新增板卡：在表中追加一项，并在 rkvc_board_id_from_name() 与
 * rkvc_detect_from_dt() 中登记名称 / device-tree 字段。
 */

static const rkvc_board_profile rk3588_profile = {
    .id            = RKVC_BOARD_RK3588,
    .name          = "rk3588",
    .soc           = "rk3588",
    .max_enc_w     = 7680,
    .max_enc_h     = 4320,
    .max_dec_w     = 7680,
    .max_dec_h     = 4320,
    .vpu_h264_enc_hw = 1,
    .vpu_hevc_enc_hw = 1,
    .vpu_av1_enc_hw  = 0,   /* RK3588 无硬件 AV1 编码（走 SVT-AV1） */
    .vpu_h264_dec_hw = 1,
    .vpu_hevc_dec_hw = 1,
    .vpu_av1_dec_hw  = 1,
    .has_npu       = 1,
    .npu_tops_x100 = 600,   /* 6 TOPS @ INT8 */
    .has_rga       = 1,
};

/*
 * RV1126B — 4K 高性能低功耗 AI 影像处理器（Quad Cortex-A53 + MCU，3 TOPS NPU）。
 * 规格取自 Rockchip 官网 RV11 系列产品页（2025），权威值：
 *   - 视频编码：4K@45fps H.264/H.265
 *   - 视频解码：4K@30fps
 *   - 无 AV1 硬件编解码；2D 图形引擎（RGA）
 */
static const rkvc_board_profile rv1126b_profile = {
    .id            = RKVC_BOARD_RV1126B,
    .name          = "rv1126b",
    .soc           = "rv1126",   /* device-tree compatible 含 rv1126 片段 */
    .max_enc_w     = 3840,
    .max_enc_h     = 2160,
    .max_dec_w     = 3840,
    .max_dec_h     = 2160,
    .vpu_h264_enc_hw = 1,
    .vpu_hevc_enc_hw = 1,
    .vpu_av1_enc_hw  = 0,
    .vpu_h264_dec_hw = 1,
    .vpu_hevc_dec_hw = 1,
    .vpu_av1_dec_hw  = 0,
    .has_npu       = 1,
    .npu_tops_x100 = 300,   /* 3.0 TOPS @ INT8 */
    .has_rga       = 1,
};

#ifndef RKVC_DEFAULT_BOARD
#define RKVC_DEFAULT_BOARD RKVC_BOARD_RK3588
#endif

const rkvc_board_profile *rkvc_board_profile_get(rkvc_board_id id)
{
    switch (id) {
    case RKVC_BOARD_RV1126B: return &rv1126b_profile;
    case RKVC_BOARD_RK3588:
    default:                 return &rk3588_profile;
    }
}

/* ── 名称 ↔ 标识 ─────────────────────────────────────────────────── */

const char *rkvc_board_id_name(rkvc_board_id id)
{
    switch (id) {
    case RKVC_BOARD_RK3588:  return "rk3588";
    case RKVC_BOARD_RV1126B: return "rv1126b";
    default:                 return "unknown";
    }
}

static rkvc_board_id name_to_id(const char *name)
{
    if (!name || !name[0])
        return RKVC_BOARD_UNKNOWN;
    if (strcmp(name, "rk3588") == 0)
        return RKVC_BOARD_RK3588;
    if (strcmp(name, "rv1126b") == 0)
        return RKVC_BOARD_RV1126B;
    return RKVC_BOARD_UNKNOWN;
}

rkvc_board_id rkvc_board_id_from_name(const char *name)
{
    if (!name)
        return RKVC_BOARD_UNKNOWN;

    char buf[32];
    size_t n = 0;
    for (; name[n] && n + 1 < sizeof(buf); n++)
        buf[n] = (char)tolower((unsigned char)name[n]);
    buf[n] = '\0';

    return name_to_id(buf);
}

/* ── 运行时探测 ──────────────────────────────────────────────────── */

static rkvc_board_id detect_from_env(void)
{
    const char *env = getenv("RKVC_BOARD");
    if (env && env[0]) {
        rkvc_board_id id = rkvc_board_id_from_name(env);
        if (id != RKVC_BOARD_UNKNOWN)
            return id;
    }
    return RKVC_BOARD_UNKNOWN;
}

/*
 * device-tree compatible 字符串以 '\0' 分隔多个条目，例如
 * RK3588: "rockchip,rk3588-evb\0rockchip,rk3588"。
 * 匹配板卡 soc 片段（如 "rk3588"、"rv1126"）即可。
 */
static rkvc_board_id detect_from_dt(void)
{
    int fd = open("/proc/device-tree/compatible", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return RKVC_BOARD_UNKNOWN;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return RKVC_BOARD_UNKNOWN;

    buf[n] = '\0';
    /*
     * compatible 以 '\0' 分隔多个条目（如 "rockchip,rk3588-evb"）。
     * 逐条目扫描是否包含板卡 soc 片段，避免依赖非标准 memmem()。
     */
    size_t off = 0;
    while (off < (size_t)n) {
        const char *entry = buf + off;
        size_t len = strlen(entry);
        if (strstr(entry, "rk3588"))
            return RKVC_BOARD_RK3588;
        if (strstr(entry, "rv1126"))
            return RKVC_BOARD_RV1126B;
        off += len + 1;   /* 跳过本条目及其结尾 '\0' */
    }
    return RKVC_BOARD_UNKNOWN;
}

rkvc_board_id rkvc_detect_board(void)
{
    static rkvc_board_id cached = RKVC_BOARD_UNKNOWN;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

    pthread_mutex_lock(&lock);
    if (cached == RKVC_BOARD_UNKNOWN) {
        rkvc_board_id id = detect_from_env();
        if (id == RKVC_BOARD_UNKNOWN)
            id = detect_from_dt();
        if (id == RKVC_BOARD_UNKNOWN)
            id = RKVC_DEFAULT_BOARD;
        cached = id;
    }
    pthread_mutex_unlock(&lock);
    return cached;
}

const rkvc_board_profile *rkvc_board_profile_active(void)
{
    return rkvc_board_profile_get(rkvc_detect_board());
}
