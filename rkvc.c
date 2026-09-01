#define _POSIX_C_SOURCE 200809L

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
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>

#ifdef __linux__
#include <unistd.h>
#endif

#include "rkvc/api.h"
#include "rkvc/diagnostic.h"
#include "rkvc/job.h"
#include "rkvc/request.h"

#define EXIT_RUNTIME 1
#define EXIT_USAGE   2

#ifdef PATH_MAX
#define RKVC_PATH_MAX PATH_MAX
#else
#define RKVC_PATH_MAX 4096
#endif

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
        "  license [--json] [--full]  显示许可证与第三方声明位置\n"
        "\n"
        "媒体子命令:\n"
        "  decode    -i in.es -o out.nv12 [--codec h264|hevc|av1]\n"
        "  encode    -i in.nv12 -o out.es --width W --height H\n"
        "            [--codec h264|hevc] [--bitrate BPS] [--qp QP]\n"
        "  transcode -i in.es -o out.es --codec h264|hevc [--bitrate BPS]\n"
        "            [--width W --height H] 转码中缩放\n"
        "  upscale   -i in.nv12 -o out.nv12 --width W --height H\n"
        "            [--model ID]   NPU 超分优先，无模型回退 RGA 2x\n"
        "  bench OP  使用上述媒体参数，另加 [--warmup N] [--iterations N]\n"
        "            [--frames N] [--duration SEC]；OP 为上述媒体子命令\n",
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

/** 找到随安装树交付的许可证或 legal 目录；结果不要求 realpath。 */
static int sibling_path(char *out, size_t out_size, const char *suffix,
                        int want_dir) {
#ifdef __linux__
    char exe[RKVC_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *slash;
    FILE *fp;
    if (n <= 0 || (size_t)n >= sizeof(exe))
        return 0;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (!slash)
        return 0;
    *slash = '\0';
    if (snprintf(out, out_size, "%s/%s", exe, suffix) >= (int)out_size)
        return 0;
    if (want_dir)
        return access(out, R_OK | X_OK) == 0;
    fp = fopen(out, "rb");
    if (!fp)
        return 0;
    fclose(fp);
    return 1;
#else
    (void)out;
    (void)out_size;
    (void)suffix;
    (void)want_dir;
    return 0;
#endif
}

/** license：稳定标识始终可用；--full 从真实安装树流式读取全文。 */
static int cmd_license(int argc, char **argv, int start, int json) {
    char license_path[RKVC_PATH_MAX] = {0};
    char legal_path[RKVC_PATH_MAX] = {0};
    int full = 0;
    int have_license, have_legal;
    int i;
    for (i = start; i < argc; ++i) {
        if (!strcmp(argv[i], "--json"))
            continue;
        if (!strcmp(argv[i], "--full")) {
            full = 1;
            continue;
        }
        fprintf(stderr, "rkvc license: 参数错误；参见 rkvc help\n");
        return EXIT_USAGE;
    }
    if (json && full) {
        fprintf(stderr, "rkvc license: --full 与 --json 不能同时使用\n");
        return EXIT_USAGE;
    }
    have_license = sibling_path(license_path, sizeof(license_path),
                                "../share/licenses/rkvc/LICENSE", 0);
    if (!have_license)
        have_license = sibling_path(license_path, sizeof(license_path),
                                    "../legal/rkvc.LICENSE", 0);
    have_legal = sibling_path(legal_path, sizeof(legal_path), "../legal", 1);
    if (json) {
        fputs("{\"project\": \"rkvc\", \"license\": "
              "\"AGPL-3.0-or-later\", \"license_file\": ", stdout);
        if (have_license)
            json_escape(stdout, license_path);
        else
            fputs("null", stdout);
        fputs(", \"third_party_notices\": ", stdout);
        if (have_legal)
            json_escape(stdout, legal_path);
        else
            fputs("null", stdout);
        fputs(", \"status\": \"ok\"}\n", stdout);
        return 0;
    }
    if (!full) {
        puts("rkvc: AGPL-3.0-or-later");
        printf("license text: %s\n",
               have_license ? license_path : "not present in this build tree");
        printf("third-party notices: %s\n",
               have_legal ? legal_path : "not present in this build tree");
        return 0;
    }
    if (!have_license) {
        fprintf(stderr, "rkvc license: 安装树中没有许可证全文\n");
        return EXIT_RUNTIME;
    }
    {
        unsigned char buffer[8192];
        FILE *fp = fopen(license_path, "rb");
        size_t n;
        if (!fp)
            return EXIT_RUNTIME;
        while ((n = fread(buffer, 1, sizeof(buffer), fp)) > 0)
            if (fwrite(buffer, 1, n, stdout) != n) {
                fclose(fp);
                return EXIT_RUNTIME;
            }
        if (ferror(fp)) {
            fclose(fp);
            return EXIT_RUNTIME;
        }
        fclose(fp);
    }
    return 0;
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

typedef struct media_options {
    const char *input;
    const char *output;
    const char *codec_text;
    const char *model;
    const char *backend_dir;
    const char *model_dir;
    long width;
    long height;
    long bitrate;
    long qp;
    rkvc_codec codec;
} media_options;

typedef struct bench_options {
    long warmup;
    long iterations;
    long frames;
    double duration;
} bench_options;

static int parse_long_value(const char *text, long min, long max, long *out) {
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end || value < min || value > max)
        return 0;
    *out = value;
    return 1;
}

static int parse_double_value(const char *text, double min, double *out) {
    char *end = NULL;
    double value;
    errno = 0;
    value = strtod(text, &end);
    if (errno || !end || *end || !isfinite(value) || value < min)
        return 0;
    *out = value;
    return 1;
}

/** 解析媒体公共参数；bench 非 NULL 时额外接受基准选项。 */
static int parse_media_options(int argc, char **argv, int start,
                               media_options *opts, bench_options *bench) {
    int i;
    memset(opts, 0, sizeof(*opts));
    opts->qp = -1;
    if (bench) {
        bench->warmup = 1;
        bench->iterations = 5;
        bench->frames = 0;
        bench->duration = 0.0;
    }
    for (i = start; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-i") || !strcmp(a, "--input")) {
            if (++i >= argc) return 0;
            opts->input = argv[i];
        } else if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
            if (++i >= argc) return 0;
            opts->output = argv[i];
        } else if (!strcmp(a, "--codec")) {
            if (++i >= argc) return 0;
            opts->codec_text = argv[i];
        } else if (!strcmp(a, "--model")) {
            if (++i >= argc) return 0;
            opts->model = argv[i];
        } else if (!strcmp(a, "--backend-dir")) {
            if (++i >= argc) return 0;
            opts->backend_dir = argv[i];
        } else if (!strcmp(a, "--model-dir")) {
            if (++i >= argc) return 0;
            opts->model_dir = argv[i];
        } else if (!strcmp(a, "--width")) {
            if (++i >= argc || !parse_long_value(argv[i], 1, UINT32_MAX,
                                                 &opts->width)) return 0;
        } else if (!strcmp(a, "--height")) {
            if (++i >= argc || !parse_long_value(argv[i], 1, UINT32_MAX,
                                                 &opts->height)) return 0;
        } else if (!strcmp(a, "--bitrate")) {
            if (++i >= argc || !parse_long_value(argv[i], 1, INT32_MAX,
                                                 &opts->bitrate)) return 0;
        } else if (!strcmp(a, "--qp")) {
            if (++i >= argc || !parse_long_value(argv[i], 0, 255,
                                                 &opts->qp)) return 0;
        } else if (bench && !strcmp(a, "--warmup")) {
            if (++i >= argc || !parse_long_value(argv[i], 0, 1000,
                                                 &bench->warmup)) return 0;
        } else if (bench && !strcmp(a, "--iterations")) {
            if (++i >= argc || !parse_long_value(argv[i], 1, 10000,
                                                 &bench->iterations)) return 0;
        } else if (bench && !strcmp(a, "--frames")) {
            if (++i >= argc || !parse_long_value(argv[i], 1, LONG_MAX,
                                                 &bench->frames)) return 0;
        } else if (bench && !strcmp(a, "--duration")) {
            if (++i >= argc || !parse_double_value(argv[i], 0.000001,
                                                   &bench->duration)) return 0;
        } else if (!strcmp(a, "--json")) {
            continue;
        } else {
            return 0;
        }
    }
    return 1;
}

static int validate_media_options(const char *name, rkvc_operation op,
                                  media_options *opts) {
    int codec_ok = 1;
    if (!opts->input || !opts->output)
        return 0;
    opts->codec = parse_codec(opts->codec_text, &codec_ok);
    if (!codec_ok)
        return 0;
    if (op == RKVC_OPERATION_DECODE && opts->codec == RKVC_CODEC_AUTO)
        opts->codec = infer_codec_from_ext(opts->input);
    if (op == RKVC_OPERATION_ENCODE &&
        (opts->codec == RKVC_CODEC_AUTO || !opts->width || !opts->height)) {
        fprintf(stderr, "rkvc %s: encode 需要 --codec 与 --width/--height\n",
                name);
        return 0;
    }
    if (op == RKVC_OPERATION_UPSCALE && (!opts->width || !opts->height)) {
        fprintf(stderr, "rkvc %s: upscale 需要 --width/--height"
                "（原始 NV12 输入几何）\n", name);
        return 0;
    }
    if (op == RKVC_OPERATION_TRANSCODE &&
        opts->codec == RKVC_CODEC_AUTO) {
        fprintf(stderr, "rkvc %s: transcode 需要 --codec 指定目标编码\n",
                name);
        return 0;
    }
    return 1;
}

/** 执行一次媒体请求；上下文/作业生命周期属于本调用。 */
static rkvc_status run_media_once(rkvc_operation op,
                                  const media_options *opts,
                                  rkvc_diag **diag) {
    rkvc_context *ctx = NULL;
    rkvc_context_options context_options;
    const char *backend_dirs[1];
    const rkvc_context_options *context_options_ptr = NULL;
    rkvc_request req;
    rkvc_job *job = NULL;
    rkvc_status st;
    if (opts->backend_dir || opts->model_dir) {
        rkvc_context_options_init(&context_options, sizeof(context_options));
        if (opts->backend_dir) {
            backend_dirs[0] = opts->backend_dir;
            context_options.paths.backend_dirs = backend_dirs;
            context_options.paths.backend_dir_count = 1;
        }
        context_options.model_dir_override = opts->model_dir;
        context_options_ptr = &context_options;
    }
    st = rkvc_context_create(context_options_ptr, &ctx);
    if (st != RKVC_STATUS_OK)
        return st;
    rkvc_request_init(&req, sizeof(req));
    req.operation = op;
    req.codec = opts->codec;
    req.input.kind = RKVC_ENDPOINT_FILE;
    req.input.uri = opts->input;
    req.output.kind = RKVC_ENDPOINT_FILE;
    req.output.uri = opts->output;
    req.width = (uint32_t)opts->width;
    req.height = (uint32_t)opts->height;
    req.quality.bitrate_bps = (int32_t)opts->bitrate;
    req.quality.qp = (int32_t)opts->qp;
    req.model_id = opts->model;
    st = rkvc_job_create(ctx, &req, diag, &job);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_start(job, diag);
    if (st == RKVC_STATUS_OK)
        st = rkvc_job_wait(job);
    rkvc_job_destroy(job);
    rkvc_context_destroy(ctx);
    return st;
}

/** 媒体子命令公共实现：解析参数 → 建请求 → create/start/wait → 汇报。 */
static int cmd_media(const char *name, rkvc_operation op,
                     int argc, char **argv, int start, int json) {
    media_options opts;
    rkvc_diag *diag = NULL;
    rkvc_status st;
    int rc = EXIT_RUNTIME;

    if (!parse_media_options(argc, argv, start, &opts, NULL) ||
        !validate_media_options(name, op, &opts))
        goto usage_err;
    st = run_media_once(op, &opts, &diag);

    if (st == RKVC_STATUS_OK) {
        if (json)
            printf("{\"command\": \"%s\", \"status\": \"ok\"}\n", name);
        else
            printf("%s: 完成 %s -> %s\n", name, opts.input, opts.output);
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
    rkvc_diag_release(diag);
    return rc;

usage_err:
    fprintf(stderr, "rkvc %s: 参数错误；参见 rkvc help\n", name);
    return EXIT_USAGE;
}

static double elapsed_seconds(const struct timespec *start,
                              const struct timespec *end) {
    return (double)(end->tv_sec - start->tv_sec) +
           (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static long infer_raw_frames(const media_options *opts, rkvc_operation op) {
    struct stat st;
    unsigned long long bytes;
    if ((op != RKVC_OPERATION_ENCODE && op != RKVC_OPERATION_UPSCALE) ||
        !opts->width || !opts->height || stat(opts->input, &st) != 0 ||
        st.st_size <= 0)
        return 0;
    bytes = (unsigned long long)opts->width * opts->height * 3 / 2;
    if (!bytes || (unsigned long long)st.st_size % bytes)
        return 0;
    if ((unsigned long long)st.st_size / bytes > LONG_MAX)
        return 0;
    return (long)((unsigned long long)st.st_size / bytes);
}

static int operation_from_name(const char *name, rkvc_operation *operation) {
    if (!strcmp(name, "decode")) *operation = RKVC_OPERATION_DECODE;
    else if (!strcmp(name, "encode")) *operation = RKVC_OPERATION_ENCODE;
    else if (!strcmp(name, "transcode")) *operation = RKVC_OPERATION_TRANSCODE;
    else if (!strcmp(name, "upscale")) *operation = RKVC_OPERATION_UPSCALE;
    else return 0;
    return 1;
}

/** bench：同一 Request/Job 路径预热并重复采样，不启动子进程。 */
static int cmd_bench(int argc, char **argv, int start, int json) {
    const char *operation_name;
    rkvc_operation operation;
    media_options media;
    bench_options bench;
    double *samples = NULL;
    double sum = 0.0, min = 0.0, max = 0.0, mean;
    long i;

    if (start >= argc || !strcmp(argv[start], "--help") ||
        !strcmp(argv[start], "-h")) {
        fputs("用法: rkvc bench OP -i INPUT -o OUTPUT [媒体参数] "
              "[--warmup N] [--iterations N] [--frames N] "
              "[--duration SEC] [--json]\n", stdout);
        return start < argc ? 0 : EXIT_USAGE;
    }
    operation_name = argv[start];
    if (!operation_from_name(operation_name, &operation) ||
        !parse_media_options(argc, argv, start + 1, &media, &bench) ||
        !validate_media_options("bench", operation, &media)) {
        fprintf(stderr, "rkvc bench: 参数错误；参见 rkvc bench --help\n");
        return EXIT_USAGE;
    }
    if (!bench.frames)
        bench.frames = infer_raw_frames(&media, operation);
    samples = calloc((size_t)bench.iterations, sizeof(*samples));
    if (!samples)
        return EXIT_RUNTIME;

    for (i = -bench.warmup; i < bench.iterations; ++i) {
        struct timespec begin, end;
        rkvc_diag *diag = NULL;
        rkvc_status st;
        clock_gettime(CLOCK_MONOTONIC, &begin);
        st = run_media_once(operation, &media, &diag);
        clock_gettime(CLOCK_MONOTONIC, &end);
        if (st != RKVC_STATUS_OK) {
            fprintf(stderr, "rkvc bench %s: 失败: %s\n",
                    operation_name, rkvc_status_str(st));
            print_diag_text(diag);
            rkvc_diag_release(diag);
            free(samples);
            return EXIT_RUNTIME;
        }
        rkvc_diag_release(diag);
        if (i >= 0)
            samples[i] = elapsed_seconds(&begin, &end);
    }
    min = max = samples[0];
    for (i = 0; i < bench.iterations; ++i) {
        sum += samples[i];
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
    }
    mean = sum / (double)bench.iterations;
    if (json) {
        printf("{\"command\": \"bench\", \"operation\": ");
        json_escape(stdout, operation_name);
        printf(", \"warmup\": %ld, \"iterations\": %ld, "
               "\"mean_seconds\": %.9f, \"min_seconds\": %.9f, "
               "\"max_seconds\": %.9f",
               bench.warmup, bench.iterations, mean, min, max);
        if (bench.frames)
            printf(", \"frames\": %ld, \"fps\": %.6f",
                   bench.frames, (double)bench.frames / mean);
        if (bench.duration > 0.0)
            printf(", \"duration_seconds\": %.6f, \"realtime\": %.6f",
                   bench.duration, bench.duration / mean);
        fputs(", \"status\": \"ok\"}\n", stdout);
    } else {
        printf("bench %s: iterations=%ld warmup=%ld mean=%.6fs "
               "min=%.6fs max=%.6fs",
               operation_name, bench.iterations, bench.warmup,
               mean, min, max);
        if (bench.frames)
            printf(" fps=%.2f", (double)bench.frames / mean);
        if (bench.duration > 0.0)
            printf(" realtime=%.2fx", bench.duration / mean);
        putchar('\n');
    }
    free(samples);
    return 0;
}

/** CLI 入口：全局 --json 预扫描 + 子命令分发。 */
int main(int argc, char **argv) {
    int json = 0;
    int i;
    const char *cmd;

    if (argc == 2 && (!strcmp(argv[1], "--help") ||
                      !strcmp(argv[1], "-h"))) {
        usage(stdout);
        return 0;
    }
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
    if (!cmd || strcmp(cmd, "help") == 0) {
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
    if (strcmp(cmd, "bench") == 0)
        return cmd_bench(argc, argv, i + 1, json);
    if (strcmp(cmd, "license") == 0)
        return cmd_license(argc, argv, i + 1, json);

    fprintf(stderr, "rkvc: 未知子命令: %s\n", cmd);
    usage(stderr);
    return EXIT_USAGE;
}
