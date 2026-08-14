/* SPDX-License-Identifier: AGPL-3.0-or-later */
/* Copyright (c) 2026 梦归云帆 */

/**
 * @file rkvc_transcode.c
 * @brief Session 转码 CLI（供 bench/ RD 基准与脚本调用）。
 *
 * 支持 MLVC 神经编解码：`--codec mlvc` + `--mlvc-enc/--mlvc-dec/--mlvc-pmf-*`
 * + 可选 `--mlvc-qp-patch-dir`（打开时打 QPP1 补丁）。
 */

#include "rkvc/rkvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

static rkvc_policy parse_policy(const char *s)
{
    if (!s || strcmp(s, "realtime") == 0) return RKVC_POLICY_REALTIME;
    if (strcmp(s, "balanced") == 0) return RKVC_POLICY_BALANCED;
    if (strcmp(s, "quality") == 0) return RKVC_POLICY_QUALITY;
    if (strcmp(s, "offline") == 0) return RKVC_POLICY_OFFLINE;
    if (strcmp(s, "neural") == 0)  return RKVC_POLICY_NEURAL;
    return RKVC_POLICY_BALANCED;
}

static int parse_rc_mode(const char *s, rkvc_rc_mode *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "vbr") == 0 || strcmp(s, "VBR") == 0) {
        *out = RKVC_RC_VBR;
        return 0;
    }
    if (strcmp(s, "cbr") == 0 || strcmp(s, "CBR") == 0) {
        *out = RKVC_RC_CBR;
        return 0;
    }
    if (strcmp(s, "cqp") == 0 || strcmp(s, "CQP") == 0 ||
        strcmp(s, "fixqp") == 0) {
        *out = RKVC_RC_CQP;
        return 0;
    }
    return -1;
}

static int parse_codec(const char *s, rkvc_codec *out)
{
    if (!s || !out)
        return -1;
    if (strcmp(s, "auto") == 0)   { *out = RKVC_CODEC_AUTO; return 0; }
    if (strcmp(s, "h264") == 0)   { *out = RKVC_CODEC_H264; return 0; }
    if (strcmp(s, "hevc") == 0)   { *out = RKVC_CODEC_HEVC; return 0; }
    if (strcmp(s, "av1") == 0)    { *out = RKVC_CODEC_AV1;  return 0; }
    if (strcmp(s, "mlvc") == 0)   { *out = RKVC_CODEC_MLVC; return 0; }
    return -1;
}

static void usage(void)
{
    fprintf(stderr,
        "usage: rkvc_transcode -i IN -o OUT [-c auto|h264|hevc|av1|mlvc]\n"
        "                      [-p realtime|balanced|quality|offline|neural]\n"
        "                      [-b bps] [-s WxH] [--rc-mode vbr|cbr|cqp]\n"
        "                      [--qp N] [--svt-lp N] [--svt-rtc]\n"
       "  MLVC 神经编解码器（与 264/265 平行，固定 640x368）:\n"
       "    编码  video → .mlvc :\n"
       "      -c mlvc -o out.mlvc --mlvc-enc models/MLVCEncoder_rk3588.rknn\n"
       "        --mlvc-gaussian-pmf models/gaussian.bin\n"
       "        --mlvc-bitest-pmf models/bitest.bin [--mlvc-qp 21]\n"
       "        [--mlvc-qp-patch-dir models/qp_patches]\n"
       "    解码  .mlvc → .yuv （原始 NV12，无再编码）:\n"
       "      -i in.mlvc -o out.yuv --mlvc-dec models/MLVCDecoder_rk3588.rknn\n"
       "        --mlvc-gaussian-pmf models/gaussian.bin\n"
       "        --mlvc-bitest-pmf models/bitest.bin\n"
       "        [--mlvc-qp-patch-dir models/qp_patches]\n"
       "    转码  .mlvc → .mp4 （MLVC 解码 + 标准编码，需 -c 指定输出编解码器）:\n"
       "      -i in.mlvc -o out.mp4 -c hevc --mlvc-dec ... --mlvc-gaussian-pmf ...\n");
}

int main(int argc, char **argv)
{
    const char *input = NULL, *output = NULL, *policy_s = "balanced";
    const char *rc_mode_s = NULL;
    const char *codec_s = NULL;
    int64_t bitrate = 4000000;
    int w = 0, h = 0, qp = -1;
    int svt_lp = 0;
    int svt_rtc = 0;

    /* MLVC 专用 */
    const char *mlvc_enc = NULL, *mlvc_dec = NULL;
    const char *mlvc_gaussian = NULL, *mlvc_bitest = NULL;
    const char *mlvc_qp_patch_dir = NULL;
    int mlvc_qp = 21;

    static struct option opts[] = {
        {"input", required_argument, 0, 'i'},
        {"output", required_argument, 0, 'o'},
        {"codec", required_argument, 0, 'c'},
        {"policy", required_argument, 0, 'p'},
        {"bitrate", required_argument, 0, 'b'},
        {"size", required_argument, 0, 's'},
        {"rc-mode", required_argument, 0, 'R'},
        {"qp", required_argument, 0, 'q'},
        {"svt-lp", required_argument, 0, 'L'},
        {"svt-rtc", no_argument, 0, 'C'},
        {"mlvc-enc", required_argument, 0, 0x1001},
        {"mlvc-dec", required_argument, 0, 0x1002},
        {"mlvc-gaussian-pmf", required_argument, 0, 0x1003},
        {"mlvc-bitest-pmf", required_argument, 0, 0x1004},
        {"mlvc-qp", required_argument, 0, 0x1005},
        {"mlvc-qp-patch-dir", required_argument, 0, 0x1006},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "i:o:c:p:b:s:R:q:L:Ch", opts, NULL)) != -1) {
        switch (c) {
        case 'i': input = optarg; break;
        case 'o': output = optarg; break;
        case 'c': codec_s = optarg; break;
        case 'p': policy_s = optarg; break;
        case 'b': bitrate = atoll(optarg); break;
        case 's': sscanf(optarg, "%dx%d", &w, &h); break;
        case 'R': rc_mode_s = optarg; break;
        case 'q': qp = atoi(optarg); break;
        case 'L': svt_lp = atoi(optarg); break;
        case 'C': svt_rtc = 1; break;
        case 0x1001: mlvc_enc = optarg; break;
        case 0x1002: mlvc_dec = optarg; break;
        case 0x1003: mlvc_gaussian = optarg; break;
        case 0x1004: mlvc_bitest = optarg; break;
        case 0x1005: mlvc_qp = atoi(optarg); break;
        case 0x1006: mlvc_qp_patch_dir = optarg; break;
        case 'h': usage(); return 0;
        default: usage(); return 1;
        }
    }

    if (!input || !output) {
        usage();
        return 1;
    }

    rkvc_codec codec = RKVC_CODEC_AUTO;
    if (codec_s) {
        if (parse_codec(codec_s, &codec) < 0) {
            fprintf(stderr, "invalid codec: %s\n", codec_s);
            return 1;
        }
    }

    /* ── MLVC 方向判定：编解码器独立选择 ──
     * MLVC 是与 264/265 平行的端到端 AI 编解码器（非转码中间件）：
     *   编码  video → .mlvc   （输出编解码器 = MLVC）
     *   解码  .mlvc  → .yuv   （输入编解码器 = MLVC，输出原始帧，无再编码）
     *   转码  .mlvc  → .mp4   （MLVC 解码 + 标准编码，-c 指定输出编解码器）*/
    size_t inlen  = strlen(input);
    size_t outlen = strlen(output);
    int in_is_mlvc  = (inlen >= 5 &&
                       strcasecmp(input + inlen - 5, ".mlvc") == 0);
    int out_is_mlvc = (outlen >= 5 &&
                       strcasecmp(output + outlen - 5, ".mlvc") == 0);
    int out_is_raw  = (outlen >= 4 &&
                       (strcasecmp(output + outlen - 4, ".yuv") == 0 ||
                        strcasecmp(output + outlen - 4, ".raw") == 0));

    /* MLVC 编解码器只能产出 .mlvc 码流 */
    if (codec == RKVC_CODEC_MLVC && !out_is_mlvc) {
        fprintf(stderr,
            "MLVC codec produces .mlvc output; specify a .mlvc output path\n");
        return 1;
    }

    int policy_is_neural = (strcmp(policy_s, "neural") == 0);
    int mlvc_involved = in_is_mlvc || out_is_mlvc ||
                        codec == RKVC_CODEC_MLVC || policy_is_neural;

    /* -p neural：自动选择 MLVC 编解码器（等价 -c mlvc）。
     * 但 MLVC 只能产出 .mlvc，若输出不是 .mlvc 则引导用户。 */
    if (policy_is_neural && !out_is_mlvc && !in_is_mlvc) {
        fprintf(stderr,
            "-p neural selects MLVC codec; output must be .mlvc\n");
        return 1;
    }

    rkvc_pipeline_desc d;
    if (out_is_mlvc) {
        /* 编码 → .mlvc */
        rkvc_pipeline_from_template(RKVC_TEMPLATE_MLVC_STORAGE, &d);
    } else if (in_is_mlvc && out_is_raw) {
        /* 纯解码 .mlvc → 原始 YUV（无再编码） */
        rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_DECODE, &d);
    } else {
        /* 标准转码（含 .mlvc → 标准容器：MLVC 解码 + 标准编码） */
        rkvc_pipeline_from_template(RKVC_TEMPLATE_FILE_TRANSCODE, &d);
    }

    d.policy      = parse_policy(policy_s);
    d.input_path  = input;
    d.output_path = output;
    d.bitrate     = bitrate;
    d.svt_lp      = svt_lp;
    d.svt_rtc     = svt_rtc;
    if (w > 0 && h > 0) {
        d.width  = w;
        d.height = h;
    }
    if (rc_mode_s) {
        if (parse_rc_mode(rc_mode_s, &d.rc_mode) < 0) {
            fprintf(stderr, "invalid rc-mode: %s\n", rc_mode_s);
            return 1;
        }
    }
    if (qp >= 0)
        d.qp_init = qp;

    /* MLVC 模型与 PMF 表（按方向校验） */
    if (mlvc_involved) {
        if (out_is_mlvc) {
            /* 编码到 .mlvc：需要 enc + PMF */
            if (!mlvc_enc || !mlvc_gaussian || !mlvc_bitest) {
                fprintf(stderr,
                    "MLVC encode requires --mlvc-enc, --mlvc-gaussian-pmf, --mlvc-bitest-pmf\n");
                return 1;
            }
            if (policy_is_neural)
                d.codec = RKVC_CODEC_AUTO;  /* 路由器按 policy=neural 自动选 MLVC */
            else
                d.codec = RKVC_CODEC_MLVC;
        } else if (in_is_mlvc) {
            /* 解码/转码 from .mlvc：需要 dec + PMF */
            if (!mlvc_dec || !mlvc_gaussian || !mlvc_bitest) {
                fprintf(stderr,
                    "MLVC decode requires --mlvc-dec, --mlvc-gaussian-pmf, --mlvc-bitest-pmf\n");
                return 1;
            }
        }
        d.mlvc_enc_model_path    = mlvc_enc;
        d.mlvc_dec_model_path    = mlvc_dec;
        d.mlvc_gaussian_pmf_path = mlvc_gaussian;
        d.mlvc_bitest_pmf_path   = mlvc_bitest;
        d.mlvc_qp                = mlvc_qp;
        d.mlvc_qp_patch_dir      = mlvc_qp_patch_dir;
    }

    rkvc_session *s = NULL;
    rkvc_err err = rkvc_session_create(&d, &s);
    if (err != RKVC_OK) {
        fprintf(stderr, "session create: %s\n", rkvc_err_str(err));
        return 1;
    }

    rkvc_route_plan plan;
    rkvc_session_get_route(s, &plan);
    fprintf(stderr, "route: %s -> %s (%s)\n",
            plan.dec_name ? plan.dec_name : "?",
            plan.enc_name ? plan.enc_name : "?",
            plan.reason ? plan.reason : "");

    err = rkvc_session_run_file(s);
    rkvc_session_destroy(s);
    if (err != RKVC_OK) {
        fprintf(stderr, "transcode failed: %s\n", rkvc_err_str(err));
        return 1;
    }
    return 0;
}
