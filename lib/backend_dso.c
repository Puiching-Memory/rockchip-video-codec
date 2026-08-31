/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file backend_dso.c
 * @brief 后端 DSO 加载器：从可信目录 dlopen 后端并完成 ABI 握手。
 *
 * 契约：后端 DSO 必须导出 `const rkvc_backend *rkvc_backend_query(void)`，
 * 返回的 rkvc_backend.abi_version 必须等于核心 RKVC_ABI_VERSION，否则淘汰。
 * 单个候选失败只淘汰该候选（计数 + 首条诊断），不影响上下文创建；候选
 * 顺序 = 内建后端 → 按路径排序的 DSO（确定性）。句柄随 context 关闭。
 */

#define _GNU_SOURCE /* dladdr/Dl_info */

#include "context_internal.h"

#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define RKVC_BACKEND_QUERY_SYMBOL "rkvc_backend_query"

typedef const rkvc_backend *(*rkvc_backend_query_fn)(void);

static int path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** 包内后端目录：<本 DSO 所在目录>/rkvc/backends */
static int package_backend_dir(char *out, size_t cap) {
    Dl_info info;
    char *slash;

    if (!dladdr((void *)rkvc_backend_count, &info) || !info.dli_fname)
        return -1;
    if (strlen(info.dli_fname) >= cap)
        return -1;
    strcpy(out, info.dli_fname);
    slash = strrchr(out, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    if (strlen(out) + strlen("/rkvc/backends") + 1 > cap)
        return -1;
    strcat(out, "/rkvc/backends");
    return 0;
}

static void collect_candidates(const char *dir, char **paths, size_t *npaths) {
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d)
        return;
    while (*npaths < RKVC_MAX_BACKENDS && (de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        char *full;
        if (len < 4 || strcmp(de->d_name + len - 3, ".so") != 0)
            continue;
        full = rkvc_g_calloc(1, strlen(dir) + len + 2);
        if (!full)
            break;
        sprintf(full, "%s/%s", dir, de->d_name);
        paths[(*npaths)++] = full;
    }
    closedir(d);
}

static void try_load(rkvc_context *ctx, const char *path) {
    void *handle;
    rkvc_backend_query_fn query;
    const rkvc_backend *be;
    rkvc_status st;

    /* RTLD_LOCAL：后端符号不污染全局命名空间；RTLD_NOW：立即解析，失败即
     * 淘汰（避免运行期惰性绑定的不可控失败点）。 */
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        snprintf(ctx->backend_diag, sizeof(ctx->backend_diag),
                 "%s: dlopen: %s", path, dlerror());
        ctx->backend_skipped++;
        return;
    }
    query = (rkvc_backend_query_fn)dlsym(handle, RKVC_BACKEND_QUERY_SYMBOL);
    if (!query) {
        snprintf(ctx->backend_diag, sizeof(ctx->backend_diag),
                 "%s: missing %s()", path, RKVC_BACKEND_QUERY_SYMBOL);
        ctx->backend_skipped++;
        dlclose(handle);
        return;
    }
    be = query();
    st = rkvc_registry_add_backend(ctx, be); /* 含 ABI 握手校验 */
    if (st != RKVC_STATUS_OK) {
        snprintf(ctx->backend_diag, sizeof(ctx->backend_diag),
                 "%s: rejected: %s", path, rkvc_status_str(st));
        ctx->backend_skipped++;
        dlclose(handle);
        return;
    }
    if (ctx->dso_count < RKVC_MAX_BACKENDS)
        ctx->dso_handles[ctx->dso_count++] = handle;
}

rkvc_status rkvc_backend_dso_scan(rkvc_context *ctx) {
    char *paths[RKVC_MAX_BACKENDS];
    size_t npaths = 0;
    char dirs[RKVC_MAX_BACKEND_DIRS][PATH_MAX];
    size_t ndirs = 0;
    size_t i;

    if (!ctx)
        return RKVC_STATUS_INVALID;

    /* 内建后端先行（确定性顺序的第一段） */
    rkvc_backend_register_builtins(ctx);

    if (package_backend_dir(dirs[0], PATH_MAX) == 0)
        ndirs = 1;
    snprintf(dirs[ndirs++], PATH_MAX, "/usr/local/lib/rkvc/backends");
    snprintf(dirs[ndirs++], PATH_MAX, "/usr/lib/rkvc/backends");
    for (i = 0; i < ctx->paths.backend_dir_count &&
                ndirs < RKVC_MAX_BACKEND_DIRS; ++i)
        snprintf(dirs[ndirs++], PATH_MAX, "%s", ctx->paths.backend_dirs[i]);

    for (i = 0; i < ndirs; ++i)
        collect_candidates(dirs[i], paths, &npaths);
    qsort(paths, npaths, sizeof(paths[0]), path_cmp);

    for (i = 0; i < npaths; ++i) {
        try_load(ctx, paths[i]);
        rkvc_g_free(paths[i]);
    }
    return RKVC_STATUS_OK;
}

void rkvc_backend_dso_close_all(rkvc_context *ctx) {
    size_t i;
    if (!ctx)
        return;
    for (i = 0; i < ctx->dso_count; ++i)
        dlclose(ctx->dso_handles[i]);
    ctx->dso_count = 0;
}
