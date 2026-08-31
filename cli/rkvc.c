/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc.c
 * @brief 0.4 单一 CLI 入口：inspect / version。
 *
 * 所有子命令共享解析、日志、退出码与 JSON 输出。媒体子命令
 * （encode/decode/transcode/upscale/bench）在后端迁入图内核后点亮；
 * 当前构建若不含媒体后端，会以统一诊断拒绝。
 *
 * 退出码：0 成功；1 运行期错误（诊断写 stderr）；2 用法错误。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkvc/api.h"

#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

static void usage(FILE *out) {
    fputs(
        "rkvc " "— Rockchip video codec toolkit (0.4)\n"
        "\n"
        "用法: rkvc <子命令> [选项]\n"
        "\n"
        "子命令:\n"
        "  inspect device [--json]    探测设备能力（SoC/VPU/RGA/NPU）\n"
        "  inspect backends [--json]  列出已装载后端\n"
        "  inspect models [--json]    列出模型注册表候选\n"
        "  version [--json]           打印库与 ABI 版本\n"
        "\n"
        "媒体子命令（待后端迁移完成后可用）:\n"
        "  encode | decode | transcode | upscale | bench | license\n",
        out);
}

/* ── 极简 JSON 输出（字段均为库内受控字符串） ────────────────────── */
static void json_escape(FILE *out, const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    fputc('"', out);
    for (; p && *p; ++p) {
        switch (*p) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (*p < 0x20)
                fprintf(out, "\\u%04x", *p);
            else
                fputc(*p, out);
        }
    }
    fputc('"', out);
}

static int cmd_version(int json) {
    if (json) {
        printf("{\"version\": ");
        json_escape(stdout, rkvc_version());
        printf(", \"abi\": %u, \"status\": \"ok\"}\n", rkvc_abi_version());
    } else {
        printf("rkvc %s (abi %u)\n", rkvc_version(), rkvc_abi_version());
    }
    return 0;
}

static int cmd_inspect_device(int json) {
    rkvc_context *ctx = NULL;
    rkvc_device_caps caps;
    rkvc_status st = rkvc_context_create(NULL, &ctx);
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc: context create failed: %s\n",
                rkvc_status_str(st));
        return EXIT_RUNTIME;
    }
    st = rkvc_probe_device(ctx, &caps);
    if (st != RKVC_STATUS_OK) {
        rkvc_context_destroy(ctx);
        fprintf(stderr, "rkvc: device probe failed: %s\n",
                rkvc_status_str(st));
        return EXIT_RUNTIME;
    }
    if (json) {
        printf("{\"soc\": ");
        json_escape(stdout, caps.soc);
        printf(", \"npu_cores\": %u, \"mpp_decoder\": %s, "
               "\"mpp_encoder\": %s, \"rga\": %s, \"rknn\": %s, "
               "\"status\": \"ok\"}\n",
               caps.npu_cores,
               caps.has_mpp_decoder ? "true" : "false",
               caps.has_mpp_encoder ? "true" : "false",
               caps.has_rga ? "true" : "false",
               caps.has_rknn ? "true" : "false");
    } else {
        printf("soc:          %s\n", caps.soc[0] ? caps.soc : "(unknown)");
        printf("npu_cores:    %u\n", caps.npu_cores);
        printf("mpp_decoder:  %s\n", caps.has_mpp_decoder ? "yes" : "no");
        printf("mpp_encoder:  %s\n", caps.has_mpp_encoder ? "yes" : "no");
        printf("rga:          %s\n", caps.has_rga ? "yes" : "no");
        printf("rknn:         %s\n", caps.has_rknn ? "yes" : "no");
    }
    rkvc_context_destroy(ctx);
    return 0;
}

static int cmd_inspect_backends(int json) {
    rkvc_context *ctx = NULL;
    rkvc_status st = rkvc_context_create(NULL, &ctx);
    size_t n, i;
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc: context create failed: %s\n",
                rkvc_status_str(st));
        return EXIT_RUNTIME;
    }
    n = rkvc_backend_count(ctx);
    if (json) {
        printf("{\"backends\": [");
        for (i = 0; i < n; ++i) {
            const char *id = rkvc_backend_id(ctx, i);
            if (i)
                fputs(", ", stdout);
            json_escape(stdout, id ? id : "");
        }
        printf("], \"status\": \"ok\"}\n");
    } else {
        if (n == 0)
            puts("(无已装载后端：核心库未扫描到后端 DSO，或未注册内建后端)");
        for (i = 0; i < n; ++i) {
            const char *id = rkvc_backend_id(ctx, i);
            printf("%s\n", id ? id : "(invalid)");
        }
    }
    rkvc_context_destroy(ctx);
    return 0;
}

static const char *trust_str(rkvc_model_trust t) {
    switch (t) {
    case RKVC_MODEL_TRUST_UNSIGNED:    return "unsigned";
    case RKVC_MODEL_TRUST_DEVELOPMENT: return "development";
    case RKVC_MODEL_TRUST_PRODUCTION:  return "production";
    default:                           return "untrusted";
    }
}

static void print_model_json(const rkvc_model_info *m, int last) {
    printf("{\"id\": ");
    json_escape(stdout, m->id);
    printf(", \"family\": ");
    json_escape(stdout, m->family);
    printf(", \"role\": ");
    json_escape(stdout, m->role);
    printf(", \"version\": ");
    json_escape(stdout, m->version);
    printf(", \"rknn_target\": ");
    json_escape(stdout, m->rknn_target);
    printf(", \"trust\": ");
    json_escape(stdout, trust_str(m->trust));
    printf(", \"payload_mask\": %u}%s", m->payload_mask, last ? "" : ", ");
}

static int cmd_inspect_models(int json) {
    rkvc_context *ctx = NULL;
    rkvc_status st = rkvc_context_create(NULL, &ctx);
    size_t n, i;
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc: context create failed: %s\n",
                rkvc_status_str(st));
        return EXIT_RUNTIME;
    }
    n = rkvc_model_count(ctx);
    if (json) {
        printf("{\"models\": [");
        for (i = 0; i < n; ++i) {
            rkvc_model_info m;
            if (rkvc_model_info_at(ctx, i, &m) == RKVC_STATUS_OK)
                print_model_json(&m, i + 1 == n);
        }
        printf("], \"status\": \"ok\"}\n");
    } else {
        if (n == 0)
            puts("(模型注册表为空：可信目录中无有效 .rkmodel 候选)");
        for (i = 0; i < n; ++i) {
            rkvc_model_info m;
            if (rkvc_model_info_at(ctx, i, &m) != RKVC_STATUS_OK)
                continue;
            printf("%s  family=%s role=%s version=%s target=%s trust=%s\n",
                   m.id, m.family, m.role, m.version,
                   m.rknn_target[0] ? m.rknn_target : "-",
                   trust_str(m.trust));
        }
    }
    rkvc_context_destroy(ctx);
    return 0;
}

static int cmd_media_unavailable(const char *name, int json) {
    if (json)
        printf("{\"command\": \"%s\", \"status\": \"error\", "
               "\"reason\": \"media backends not built into this package\"}\n",
               name);
    else
        fprintf(stderr,
                "rkvc %s: 本构建未包含媒体后端（核心库 + 后端迁移进行中，见 P3）\n",
                name);
    return EXIT_RUNTIME;
}

int main(int argc, char **argv) {
    int json = 0;
    int i;
    const char *cmd;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0)
            json = 1;
    }
    /* 找到第一个非选项实参作为子命令 */
    cmd = NULL;
    for (i = 1; i < argc; ++i) {
        if (argv[i][0] != '-') {
            cmd = argv[i];
            break;
        }
    }
    if (!cmd || strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 ||
        strcmp(cmd, "-h") == 0) {
        usage(stdout);
        return cmd ? 0 : EXIT_USAGE;
    }

    if (strcmp(cmd, "version") == 0)
        return cmd_version(json);

    if (strcmp(cmd, "inspect") == 0) {
        const char *what = NULL;
        for (++i; i < argc; ++i) {
            if (argv[i][0] != '-') {
                what = argv[i];
                break;
            }
        }
        if (!what) {
            fprintf(stderr, "rkvc: inspect 需要对象: device|backends|models\n");
            return EXIT_USAGE;
        }
        if (strcmp(what, "device") == 0)
            return cmd_inspect_device(json);
        if (strcmp(what, "backends") == 0)
            return cmd_inspect_backends(json);
        if (strcmp(what, "models") == 0)
            return cmd_inspect_models(json);
        fprintf(stderr, "rkvc: 未知 inspect 对象: %s\n", what);
        return EXIT_USAGE;
    }

    if (strcmp(cmd, "encode") == 0 || strcmp(cmd, "decode") == 0 ||
        strcmp(cmd, "transcode") == 0 || strcmp(cmd, "upscale") == 0 ||
        strcmp(cmd, "bench") == 0 || strcmp(cmd, "license") == 0)
        return cmd_media_unavailable(cmd, json);

    fprintf(stderr, "rkvc: 未知子命令: %s\n", cmd);
    usage(stderr);
    return EXIT_USAGE;
}
