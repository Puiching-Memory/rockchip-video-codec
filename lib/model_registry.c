/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file model_registry.c
 * @brief 模型注册表：在 context 创建时扫描可信目录中的 .rkmodel 候选。
 *
 * 只读有界头部并做签名/兼容性过滤；载荷按需装载。扫描确定性：路径排序
 * 后逐个解析，单个候选失败只淘汰该候选并计数（skips），不影响上下文创建。
 * 目录名只用于组织文件，不参与正确性。
 */

#define _GNU_SOURCE /* dladdr/Dl_info */

#include "context_internal.h"
#include "rkmodel.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** 推导出包内模型目录：<本 DSO 所在目录>/../share/rkvc/models */
static int package_model_dir(char *out, size_t cap) {
    Dl_info info;
    char *slash;

    if (!dladdr((void *)rkvc_model_count, &info) || !info.dli_fname)
        return -1;
    if (strlen(info.dli_fname) >= cap)
        return -1;
    strcpy(out, info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (strlen(out) + strlen("/../share/rkvc/models") + 1 > cap)
        return -1;
    strcat(out, "/../share/rkvc/models");
    return 0;
}

static void scan_dir(rkvc_context *ctx, const char *dir, char **paths,
                     size_t *npaths) {
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d)
        return;
    while (*npaths < RKMODEL_MAX_SCAN_FILES && (de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        char *full;
        if (len < 9 || strcmp(de->d_name + len - 8, ".rkmodel") != 0)
            continue;
        full = rkvc_g_calloc(1, strlen(dir) + len + 2);
        if (!full)
            break;
        sprintf(full, "%s/%s", dir, de->d_name);
        paths[(*npaths)++] = full;
    }
    closedir(d);
}

rkvc_status rkvc_model_registry_scan(rkvc_context *ctx) {
    char *paths[RKMODEL_MAX_SCAN_FILES];
    size_t npaths = 0;
    char dirs[RKVC_MAX_MODEL_DIRS][PATH_MAX];
    size_t ndirs = 0;
    size_t i;

    if (!ctx)
        return RKVC_STATUS_INVALID;
    if (package_model_dir(dirs[0], PATH_MAX) == 0)
        ndirs = 1;
    snprintf(dirs[ndirs++], PATH_MAX, "/usr/local/share/rkvc/models");
    snprintf(dirs[ndirs++], PATH_MAX, "/usr/share/rkvc/models");
    for (i = 0; i < ctx->paths.model_dir_count && ndirs < RKVC_MAX_MODEL_DIRS;
         ++i) {
        snprintf(dirs[ndirs++], PATH_MAX, "%s", ctx->paths.model_dirs[i]);
    }

    for (i = 0; i < ndirs; ++i)
        scan_dir(ctx, dirs[i], paths, &npaths);
    qsort(paths, npaths, sizeof(paths[0]), path_cmp);

    ctx->models = rkvc_g_calloc(npaths ? npaths : 1, sizeof(*ctx->models));
    if (!ctx->models) {
        for (i = 0; i < npaths; ++i)
            rkvc_g_free(paths[i]);
        return RKVC_STATUS_NOMEM;
    }
    for (i = 0; i < npaths; ++i) {
        char err[160] = {0};
        rkvc_rkmodel *m = &ctx->models[ctx->model_count];
        if (rkvc_rkmodel_open(paths[i], m, rkvc_model_trust_verifier(), NULL,
                              err, sizeof(err)) == RKVC_STATUS_OK) {
            /* 生产信任模式下 unsigned 等价 untrusted */
            if (rkvc_model_trust_production_mode() &&
                m->info.trust == RKVC_MODEL_TRUST_UNSIGNED)
                m->info.trust = RKVC_MODEL_TRUST_UNTRUSTED;
            ctx->model_count++;
        } else {
            ctx->model_skipped++;
        }
        rkvc_g_free(paths[i]);
    }
    return RKVC_STATUS_OK;
}

size_t rkvc_model_count(const rkvc_context *ctx) {
    return ctx ? ctx->model_count : 0;
}

rkvc_status rkvc_model_info_at(const rkvc_context *ctx, size_t idx,
                               rkvc_model_info *info) {
    if (!ctx || !info)
        return RKVC_STATUS_INVALID;
    if (idx >= ctx->model_count)
        return RKVC_STATUS_NOT_FOUND;
    *info = ctx->models[idx].info;
    return RKVC_STATUS_OK;
}
