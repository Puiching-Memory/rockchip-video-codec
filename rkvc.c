/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc.c
 * @brief 单一 CLI 入口：inspect / version / 媒体子命令。
 *
 * 所有子命令共享解析、日志、退出码与 JSON 输出。CLI 把参数转换为
 * rkvc_request 后即调用公共 API，不包含后端选择和模型路径拼装逻辑。
 *
 * 退出码：0 成功；1 运行期错误（诊断写 stderr）；2 用法错误。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rkvc/api.h"
#include "rkvc/diagnostic.h"
#include "rkvc/job.h"
#include "rkvc/request.h"

#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

/** 输出用法说明到 out（stdout 或 stderr）。 */
static void usage(FILE *out) {
    fputs(
        "rkvc " "— Rockchip video codec toolkit\n"
        "\n"
        "用法: rkvc <子命令> [选项]\n"
        "\n"
        "子命令:\n"
        "  inspect device [--json]    探测设备能力（SoC/VPU/RGA/NPU）\n"
        "  inspect backends [--json]  列出已装载后端\n"
        "  inspect models [--json]    列出模型注册表候选\n"
        "  version [--json]           打印库与 ABI 版本\n"
        "\n"
        "媒体子命令:\n"
        "  decode    -i in.es -o out.nv12 [--codec h264|hevc|av1]\n"
        "  encode    -i in.nv12 -o out.es --width W --height H\n"
        "            [--codec h264|hevc] [--bitrate BPS] [--qp QP]\n"
        "  transcode -i in.es -o out.es --codec h264|hevc [--bitrate BPS]\n"
        "            [--width W --height H] 转码中缩放\n"
        "  upscale   -i in.nv12 -o out.nv12 --width W --height H\n"
        "            [--model ID]   NPU 超分优先，无模型回退 RGA 2x\n",
        out);
}

/* ── 极简 JSON 输出（字段均为库内受控字符串） ────────────────────── */
/** 输出一个 JSON 字符串字面量（转义控制字符与引号）。 */
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

/** version 子命令：打印库与 ABI 版本。 */
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

/** inspect device 子命令：创建上下文并输出设备能力。 */
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

/** inspect backends 子命令：列出已装载后端 id。 */
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

/** 信任级别转稳定字符串（文本与 JSON 输出共用）。 */
static const char *trust_str(rkvc_model_trust t) {
    switch (t) {
    case RKVC_MODEL_TRUST_UNSIGNED:    return "unsigned";
    case RKVC_MODEL_TRUST_DEVELOPMENT: return "development";
    case RKVC_MODEL_TRUST_PRODUCTION:  return "production";
    default:                           return "untrusted";
    }
}

/** 输出一个模型候选的 JSON 对象（last 控制数组分隔符）。 */
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

/** inspect models 子命令：列出模型注册表候选摘要。 */
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

/** 未构建进本二进制的媒体子命令（upscale/bench/license）统一拒绝。 */
static int cmd_media_unavailable(const char *name, int json) {
    if (json)
        printf("{\"command\": \"%s\", \"status\": \"error\", "
               "\"reason\": \"command unavailable\"}\n",
               name);
    else
        fprintf(stderr,
                "rkvc %s: 该子命令不可用\n", name);
    return EXIT_RUNTIME;
}

/* ── 媒体子命令（decode/encode/transcode） ───────────────────────── */

/** 解析 --codec 参数（h264/hevc/av1 及常见别名）；失败置 *ok=0。 */
static rkvc_codec parse_codec(const char *s, int *ok) {
    *ok = 1;
    if (!s || strcmp(s, "auto") == 0) return RKVC_CODEC_AUTO;
    if (strcmp(s, "h264") == 0 || strcmp(s, "avc") == 0)
        return RKVC_CODEC_H264;
    if (strcmp(s, "hevc") == 0 || strcmp(s, "h265") == 0)
        return RKVC_CODEC_HEVC;
    if (strcmp(s, "av1") == 0) return RKVC_CODEC_AV1;
    *ok = 0;
    return RKVC_CODEC_AUTO;
}

/** 由输入扩展名推断解码/转码输入编码；识别不了则 AUTO（后端嗅探）。 */
static rkvc_codec infer_codec_from_ext(const char *path) {
    const char *dot = path ? strrchr(path, '.') : NULL;
    if (!dot)
        return RKVC_CODEC_AUTO;
    if (!strcmp(dot, ".h264") || !strcmp(dot, ".264"))
        return RKVC_CODEC_H264;
    if (!strcmp(dot, ".h265") || !strcmp(dot, ".hevc") ||
        !strcmp(dot, ".265"))
        return RKVC_CODEC_HEVC;
    if (!strcmp(dot, ".av1") || !strcmp(dot, ".ivf"))
        return RKVC_CODEC_AV1;
    return RKVC_CODEC_AUTO;
}

/** 把诊断链格式化为一行并打到 stderr。 */
static void print_diag_text(const rkvc_diag *diag) {
    char buf[1024];
    if (!diag)
        return;
    rkvc_diag_fmt_text(diag, buf, sizeof(buf));
    if (buf[0])
        fprintf(stderr, "rkvc: %s\n", buf);
}

/** 媒体子命令公共实现：解析参数 → 建请求 → create/start/wait → 汇报。 */
static int cmd_media(const char *name, rkvc_operation op,
                     int argc, char **argv, int start, int json) {
    const char *input = NULL, *output = NULL, *codec_s = NULL;
    const char *model = NULL;
    long width = 0, height = 0, bitrate = 0, qp = -1;
    int codec_ok = 1;
    rkvc_codec codec;
    rkvc_context *ctx = NULL;
    rkvc_request req;
    rkvc_diag *diag = NULL;
    rkvc_job *job = NULL;
    rkvc_status st;
    int i, rc = EXIT_RUNTIME;

    for (i = start; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") || !strcmp(a, "--input")) {
            if (++i >= argc) goto usage_err;
            input = argv[i];
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (++i >= argc) goto usage_err;
            output = argv[i];
        } else if (!strcmp(a, "--codec")) {
            if (++i >= argc) goto usage_err;
            codec_s = argv[i];
        } else if (!strcmp(a, "--model")) {
            if (++i >= argc) goto usage_err;
            model = argv[i];
        } else if (!strcmp(a, "--width")) {
            if (++i >= argc) goto usage_err;
            width = strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--height")) {
            if (++i >= argc) goto usage_err;
            height = strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--bitrate")) {
            if (++i >= argc) goto usage_err;
            bitrate = strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--qp")) {
            if (++i >= argc) goto usage_err;
            qp = strtol(argv[i], NULL, 10);
        } else if (!strcmp(a, "--json")) {
            /* 已在全局扫描中处理 */
        } else {
            goto usage_err;
        }
    }

    if (!input || !output)
        goto usage_err;
    codec = parse_codec(codec_s, &codec_ok);
    if (!codec_ok)
        goto usage_err;
    if (op == RKVC_OPERATION_DECODE && codec == RKVC_CODEC_AUTO)
        codec = infer_codec_from_ext(input); /* AUTO 时由后端首帧嗅探 */
    if (op == RKVC_OPERATION_ENCODE &&
        (codec == RKVC_CODEC_AUTO || width <= 0 || height <= 0)) {
        fprintf(stderr, "rkvc %s: encode 需要 --codec 与 --width/--height\n",
                name);
        goto usage_err;
    }
    if (op == RKVC_OPERATION_UPSCALE && (width <= 0 || height <= 0)) {
        fprintf(stderr, "rkvc %s: upscale 需要 --width/--height"
                "（原始 NV12 输入几何）\n", name);
        goto usage_err;
    }
    if (op == RKVC_OPERATION_TRANSCODE && codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "rkvc %s: transcode 需要 --codec 指定目标编码\n",
                name);
        goto usage_err;
    }

    st = rkvc_context_create(NULL, &ctx);
    if (st != RKVC_STATUS_OK) {
        fprintf(stderr, "rkvc: context create failed: %s\n",
                rkvc_status_str(st));
        return EXIT_RUNTIME;
    }

    rkvc_request_init(&req, sizeof(req));
    req.operation = op;
    req.codec = codec;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = input;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = output;
    req.width = (uint32_t)width;
    req.height = (uint32_t)height;
    req.quality.bitrate_bps = (int32_t)bitrate;
    req.quality.qp = (int32_t)qp;
    req.model_id = model;

    st = rkvc_job_create(ctx, &req, &diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, &diag);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);

    if (st == RKVC_STATUS_OK) {
        if (json)
            printf("{\"command\": \"%s\", \"status\": \"ok\"}\n", name);
        else
            printf("%s: 完成 %s -> %s\n", name, input, output);
        rc = 0;
    } else {
        if (json)
            printf("{\"command\": \"%s\", \"status\": \"error\", "
                   "\"reason\": \"%s\"}\n", name, rkvc_status_str(st));
        else
            fprintf(stderr, "rkvc %s: 失败: %s\n", name,
                    rkvc_status_str(st));
        print_diag_text(diag);
    }

    rkvc_job_destroy(job);
    rkvc_diag_release(diag);
    rkvc_context_destroy(ctx);
    return rc;

usage_err:
    fprintf(stderr, "rkvc %s: 参数错误；参见 rkvc help\n", name);
    return EXIT_USAGE;
}

/** CLI 入口：全局 --json 预扫描 + 子命令分发。 */
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

    if (strcmp(cmd, "decode") == 0)
        return cmd_media(cmd, RKVC_OPERATION_DECODE, argc, argv, i + 1,
                         json);
    if (strcmp(cmd, "encode") == 0)
        return cmd_media(cmd, RKVC_OPERATION_ENCODE, argc, argv, i + 1,
                         json);
    if (strcmp(cmd, "transcode") == 0)
        return cmd_media(cmd, RKVC_OPERATION_TRANSCODE, argc, argv, i + 1,
                         json);
    if (strcmp(cmd, "upscale") == 0)
        return cmd_media(cmd, RKVC_OPERATION_UPSCALE, argc, argv, i + 1,
                         json);

    if (strcmp(cmd, "bench") == 0 || strcmp(cmd, "license") == 0)
        return cmd_media_unavailable(cmd, json);

    fprintf(stderr, "rkvc: 未知子命令: %s\n", cmd);
    usage(stderr);
    return EXIT_USAGE;
}
