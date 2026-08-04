/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file test_fixtures.h
 * @brief 硬件/集成测试用自生成夹具（不依赖 git 内嵌媒体文件）。
 */

#ifndef RKVC_TEST_FIXTURES_H
#define RKVC_TEST_FIXTURES_H

#include <stddef.h>

/** 写入 NV12 测试图案（Y 渐变 + UV=128），返回 0 成功。 */
int rkvc_test_write_nv12_pattern(const char *path, int w, int h, int frames);

/**
 * 若 path 不存在则自 NV12 经 Session 编码生成 MP4（REALTIME/H.264）。
 * path 缓冲区至少 PATH_MAX；返回 0 成功。
 */
int rkvc_test_ensure_h264_mp4(char *path, size_t path_sz,
                              int w, int h, int frames);

/** 返回已生成夹具路径；首次调用时创建。w/h/frames 仅用于首次生成。 */
const char *rkvc_test_fixture_h264_mp4(int w, int h, int frames);

/** 返回 1080p NV12 夹具路径（供 3× 上采样测试）。 */
const char *rkvc_test_fixture_nv12_1080p(void);

#endif /* RKVC_TEST_FIXTURES_H */
