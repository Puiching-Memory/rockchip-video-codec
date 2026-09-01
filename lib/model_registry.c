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
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* 以对象地址定位当前 ELF 映像，避免 ISO C 禁止的函数指针到 void * 强转。 */
static const unsigned char rkvc_model_registry_anchor;

/** qsort 比较器：按路径字典序（保证扫描顺序确定）。 */
static int path_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** 推导出包内模型目录：<本 DSO 所在目录>/../share/rkvc/models */
static int package_model_dir(char *out, size_t cap) {
    Dl_info info;
    char *slash;

    if (!dladdr(&rkvc_model_registry_anchor, &info) || !info.dli_fname)
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

/** 递归收集目录下 .rkmodel 文件（不跟随符号链接；深度/数量有界）。 */
static void scan_dir_recursive(const char *dir, unsigned depth, char **paths,
                               size_t *npaths) {
    DIR *d = opendir(dir);
    struct dirent *de;

    if (!d || depth > 16)
        return;
    while (*npaths < RKMODEL_MAX_SCAN_FILES && (de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        char *full;
        struct stat st;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        full = rkvc_g_calloc(1, strlen(dir) + len + 2);
        if (!full)
            break;
        sprintf(full, "%s/%s", dir, de->d_name);
        /* 发现可信内容时不跟随符号链接。 */
        if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) {
            rkvc_g_free(full);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            scan_dir_recursive(full, depth + 1, paths, npaths);
            rkvc_g_free(full);
            continue;
        }
        if (S_ISREG(st.st_mode) && len >= 9 &&
            strcmp(de->d_name + len - 8, ".rkmodel") == 0)
            paths[(*npaths)++] = full;
        else
            rkvc_g_free(full);
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
    if (ctx->model_dir_override) {
        snprintf(dirs[ndirs++], PATH_MAX, "%s", ctx->model_dir_override);
    } else {
        if (package_model_dir(dirs[0], PATH_MAX) == 0)
            ndirs = 1;
        snprintf(dirs[ndirs++], PATH_MAX, "/usr/local/share/rkvc/models");
        snprintf(dirs[ndirs++], PATH_MAX, "/usr/share/rkvc/models");
        for (i = 0; i < ctx->paths.model_dir_count &&
                    ndirs < RKVC_MAX_MODEL_DIRS; ++i)
            snprintf(dirs[ndirs++], PATH_MAX, "%s", ctx->paths.model_dirs[i]);
    }

    for (i = 0; i < ndirs; ++i)
        scan_dir_recursive(dirs[i], 0, paths, &npaths);
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
            int reject = 0;
            /* 签名存在但验证不通过的候选绝不可用；
             * 生产模式额外要求生产 trust root 签名。 */
            if (m->info.trust == RKVC_MODEL_TRUST_UNTRUSTED)
                reject = 1;
            if (rkvc_model_trust_production_mode() &&
                m->info.trust != RKVC_MODEL_TRUST_PRODUCTION)
                reject = 1;
            if (m->min_runtime_abi > RKVC_ABI_VERSION)
                reject = 1;
            if (reject)
                ctx->model_skipped++;
            else
                ctx->model_count++;
        } else {
            ctx->model_skipped++;
        }
        rkvc_g_free(paths[i]);
    }
    return RKVC_STATUS_OK;
}

/** 请求类型对应的模型角色；TRANSCODE 无单一角色（返回 NULL = 不过滤）。 */
static const char *role_for_operation(rkvc_operation op) {
    switch (op) {
    case RKVC_OPERATION_ENCODE:  return "encoder";
    case RKVC_OPERATION_DECODE:  return "decoder";
    case RKVC_OPERATION_UPSCALE: return "upscale";
    default:                     return NULL;
    }
}

const rkvc_rkmodel *rkvc_model_registry_select(const rkvc_context *ctx,
                                               const rkvc_request *req,
                                               rkvc_diag **diag) {
    const rkvc_rkmodel *best = NULL;
    const char *role;
    int best_score = -1;
    size_t i;
    if (!ctx || !req)
        return NULL;
    role = role_for_operation(req->operation);
    for (i = 0; i < ctx->model_count; ++i) {
        const rkvc_rkmodel *m = &ctx->models[i];
        int score = 0;
        if (req->model_id && strcmp(req->model_id, m->info.id) != 0)
            continue;
        if (!req->model_id && role && strcmp(role, m->info.role) != 0)
            continue;
        if (m->info.rknn_target[0] && ctx->caps.soc[0] &&
            strcmp(m->info.rknn_target, ctx->caps.soc) != 0)
            continue;
        if (m->info.trust == RKVC_MODEL_TRUST_PRODUCTION) score += 300;
        else if (m->info.trust == RKVC_MODEL_TRUST_DEVELOPMENT) score += 200;
        else score += 100;
        if (m->info.rknn_target[0] && ctx->caps.soc[0]) score += 50;
        if (!best || score > best_score ||
            (score == best_score && strcmp(m->info.id, best->info.id) < 0)) {
            best = m;
            best_score = score;
        }
    }
    if (!best && diag)
        rkvc_diag_push(diag, RKVC_STATUS_NOT_FOUND, 2,
                       "model-registry", "no compatible model candidate");
    return best;
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
