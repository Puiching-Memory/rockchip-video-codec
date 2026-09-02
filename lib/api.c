/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file api.c
 * @brief 公共 ABI 效用：版本、状态码字符串、初始化与诊断链。
 *
 * 这些函数不依赖后端或图内核，可独立链接；用于 CLI / 应用层。
 */

#include "graph_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 库内分配器：集中在此，便于将来替换为池化/审计分配器；
 * 单测（RKVC_STANDALONE_TEST）直接映射到 libc，不经过本实现。 */
#ifndef RKVC_STANDALONE_TEST
void *rkvc_calloc(size_t nmemb, size_t size) {
    return calloc(nmemb, size);
}

void rkvc_free(void *ptr) {
    free(ptr);
}
#endif

#ifndef RKVC_VERSION_STR
#define RKVC_VERSION_STR "0.4.0-dev"
#endif

const char *rkvc_version(void) {
    return RKVC_VERSION_STR;
}

uint32_t rkvc_abi_version(void) {
    return RKVC_ABI_VERSION;
}

const char *rkvc_status_str(rkvc_status status) {
    switch (status) {
    case RKVC_STATUS_OK:              return "ok";
    case RKVC_STATUS_NOMEM:           return "out of memory";
    case RKVC_STATUS_INVALID:         return "invalid argument";
    case RKVC_STATUS_NOT_FOUND:       return "not found";
    case RKVC_STATUS_IO:              return "i/o error";
    case RKVC_STATUS_HW:              return "hardware init failed";
    case RKVC_STATUS_EOF:             return "end of stream";
    case RKVC_STATUS_AGAIN:           return "would block (again)";
    case RKVC_STATUS_FORMAT:          return "format mismatch";
    case RKVC_STATUS_NEGOTIATE:       return "graph negotiation failed";
    case RKVC_STATUS_PERMISSION:      return "permission denied";
    case RKVC_STATUS_LICENSE:         return "license check failed";
    case RKVC_STATUS_UNLICENSED:      return "unlicensed";
    case RKVC_STATUS_CANCELED:        return "canceled";
    case RKVC_STATUS_UNSUPPORTED:     return "unsupported";
    case RKVC_STATUS_INTERNAL:        return "internal error";
    case RKVC_STATUS_INTEGRITY:       return "integrity check failed";
    default:                          return "unknown";
    }
}

void rkvc_request_init(rkvc_request *req, size_t size) {
    if (!req || size < sizeof(rkvc_header))
        return;
    memset(req, 0, size);
    req->header.struct_size = size;
    req->header.api_version = RKVC_ABI_VERSION;
    req->codec = RKVC_CODEC_AUTO;
    req->policy = RKVC_POLICY_BALANCED;
    req->quality.qp = -1;
}

void rkvc_context_options_init(rkvc_context_options *opts, size_t size) {
    if (!opts || size < sizeof(rkvc_header))
        return;
    memset(opts, 0, size);
    opts->header.struct_size = size;
    opts->header.api_version = RKVC_ABI_VERSION;
    opts->thread_model = RKVC_THREAD_MODEL_DEFAULT;
    opts->inspect_timeout_ms = 2000;
}

/* ── 诊断链 ───────────────────────────────────────────────────────── */

/** 诊断节点内联字符串容量（NUL 含；subject/reason 截断到该长度）。 */
#define RKVC_DIAG_TEXT_MAX 96

void rkvc_diag_push(rkvc_diag **diag, rkvc_status status, int stage,
                    const char *subject, const char *reason) {
    /* 字符串随节点拷贝：后端 DSO 可能在诊断消费前被 dlclose，
     * 静态串指针会随映射卸载失效（板上实测崩溃）。 */
    struct rkvc_diag_owned {
        rkvc_diag node;
        char subject_text[RKVC_DIAG_TEXT_MAX];
        char reason_text[RKVC_DIAG_TEXT_MAX];
    };
    struct rkvc_diag_owned *n =
        rkvc_g_calloc(1, sizeof(*n));
    if (!n)
        return; /* 内存不足：链保持原样，调用方仍可见更上层的错误 */
    n->node.status = status;
    n->node.stage = stage;
    if (subject)
        snprintf(n->subject_text, RKVC_DIAG_TEXT_MAX, "%s", subject);
    if (reason)
        snprintf(n->reason_text, RKVC_DIAG_TEXT_MAX, "%s", reason);
    n->node.subject = n->subject_text;
    n->node.reason = n->reason_text;
    n->node.next = *diag;
    *diag = &n->node;
}

void rkvc_diag_release(rkvc_diag *diag) {
    while (diag) {
        rkvc_diag *next = (rkvc_diag *)diag->next;
        rkvc_g_free(diag); /* push 时按内联串结构整体分配 */
        diag = next;
    }
}

void rkvc_diag_fmt_text(const rkvc_diag *diag, char *buf, size_t size) {
    size_t off = 0;
    while (diag && off < size - 1) {
        int n = snprintf(buf + off, size - off, "%s(%s): %s; ",
                         diag->subject ? diag->subject : "?",
                         diag->reason ? diag->reason : "?",
                         rkvc_status_str(diag->status));
        if (n < 0)
            break;
        if ((size_t)n >= size - off)
            break;
        off += (size_t)n;
        diag = diag->next;
    }
    buf[size - 1] = '\0';
}

void rkvc_diag_fmt_json(const rkvc_diag *diag, char *buf, size_t size) {
    size_t off = 0;
    int first = 1;
    buf[0] = '\0';
    off += (size_t)snprintf(buf + off, size - off, "[");
    while (diag && off < size - 8) {
        if (!first)
            off += (size_t)snprintf(buf + off, size - off, ",");
        first = 0;
        off += (size_t)snprintf(buf + off, size - off,
            "{\"status\":\"%s\",\"sub\":\"%s\",\"why\":\"%s\"}",
            rkvc_status_str(diag->status),
            diag->subject ? diag->subject : "",
            diag->reason ? diag->reason : "");
        diag = diag->next;
    }
    off += (size_t)snprintf(buf + off, size - off, "]");
}
