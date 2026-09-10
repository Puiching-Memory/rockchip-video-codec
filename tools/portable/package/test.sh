#!/bin/bash
# rkvc @VERSION@ 可移植包自测：布局、完整性、依赖闭包、插件握手与编解码冒烟。
#
# 用法: ./test.sh
#       RKVC_RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu" ./test.sh
#
# 硬件相关项（MPP/NPU）在缺设备节点时自动跳过；任一必测项失败即非零退出。
set -uo pipefail

cd "$(dirname "$0")"
ROOT="$PWD"
BIN="$ROOT/bin/rkvc"
BACKENDS="$ROOT/lib/rkvc/backends"
VERSION="@VERSION@"

# shellcheck disable=SC2206  # 有意分词：执行器前缀可以是多条命令
RUNNER=(${RKVC_RUNNER:-})
run() { "${RUNNER[@]}" "$BIN" "$@"; }

pass=0
fail=0
skipped=0
ok() { pass=$((pass + 1)); printf '  ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$1"; }
skip() { skipped=$((skipped + 1)); printf '  skip  %s\n' "$1"; }
run_ok() { # <描述> <命令...>
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then ok "$desc"; else bad "$desc"; fi
}
grep_out() { # <描述> <模式> <命令...>
    local desc="$1" pat="$2" out
    shift 2
    if out="$("$@" 2>&1)"; then
        if grep -qE "$pat" <<<"$out"; then
            ok "$desc"
        else
            bad "$desc（输出不匹配 /$pat/）"
        fi
    else
        bad "$desc（命令失败）"
    fi
}

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

echo "== 布局 =="
if [[ -x "$BIN" ]]; then ok "bin/rkvc 可执行"; else bad "bin/rkvc 可执行"; fi
mapfile -t plugins < <(ls "$BACKENDS"/*.so 2>/dev/null)
if ((${#plugins[@]} >= 3)); then
    ok "插件 ${#plugins[@]} 个（≥3）"
else
    bad "插件数量（${#plugins[@]}，需 ≥3）"
fi
for f in README.md MANIFEST.sha256 licenses/PROVENANCE.txt; do
    if [[ -s "$ROOT/$f" ]]; then ok "$f 存在"; else bad "$f 存在"; fi
done
docs_pages="$(find "$ROOT/docs" -name '*.md' 2>/dev/null | wc -l)"
if ((docs_pages >= 10)); then
    ok "docs/ 文档 $docs_pages 个页面"
else
    bad "docs/ 文档页面数（$docs_pages，需 ≥10）"
fi
# 图片走 git-lfs：指针文件只有 130 字节，是真图就远大于此。
if [[ -s "$ROOT/docs/images/mlvc-architecture.png" ]] &&
    ! grep -q '^version https://git-lfs' \
        "$ROOT/docs/images/mlvc-architecture.png"; then
    ok "docs/ 图片为真实文件（非 lfs 指针）"
else
    bad "docs/ 图片为 git-lfs 指针"
fi
samples="$(find "$ROOT/examples" -name CMakeLists.txt 2>/dev/null | wc -l)"
if ((samples == 3)); then
    ok "examples/ 三个 C ABI 样板"
else
    bad "examples/ 样板数（$samples，需 3）"
fi

echo "== 完整性 =="
if command -v sha256sum >/dev/null 2>&1; then
    if (cd "$ROOT" && sha256sum -c MANIFEST.sha256) >/dev/null 2>&1; then
        ok "MANIFEST.sha256 校验通过"
    else
        bad "MANIFEST.sha256 校验"
    fi
else
    skip "MANIFEST 校验（无 sha256sum）"
fi

echo "== 依赖解析 =="
if ((${#RUNNER[@]} == 0)) && command -v ldd >/dev/null 2>&1; then
    for f in "$BIN" "$BACKENDS"/*.so; do
        if ldd "$f" 2>&1 | grep -q 'not found'; then
            bad "$(basename "$f") 有未解析依赖"
        else
            ok "$(basename "$f") 依赖可解析"
        fi
    done
else
    skip "ldd（模拟运行或无 ldd）"
fi

echo "== CLI =="
grep_out "version 报 $VERSION" "rkvc $VERSION" run version
run_ok "version --json" run version --json
run_ok "caps" run caps

echo "== 插件握手 =="
# inspect backends 只扫显式给的目录；插件自动发现（bin/../lib/rkvc/backends）
# 由下面的编码冒烟验证——那条路径走的是 context 的发现顺序。
if out="$(run inspect backends --backend-dir "$BACKENDS" 2>&1)"; then
    n="$(grep -cE 'rkvc_[a-z0-9]+\.so' <<<"$out")"
    if [[ "$n" == "${#plugins[@]}" ]]; then
        ok "inspect backends --backend-dir（$n/${#plugins[@]}）"
    else
        bad "inspect backends --backend-dir 装载 $n/${#plugins[@]}"
    fi
else
    bad "inspect backends --backend-dir 执行"
fi

echo "== SDK（预编译核心库，供上游直接链接）=="
for f in include/rkvc/rkvc.h include/rkvc/plugin.hpp lib/librkvc.so \
    lib/cmake/rkvc/rkvcConfig.cmake lib/cmake/rkvc/rkvcConfigVersion.cmake \
    lib/pkgconfig/rkvc.pc; do
    if [[ -s "$ROOT/$f" ]]; then ok "$f"; else bad "$f 缺失"; fi
done
# 前缀必须由文件位置反推，整包搬走后仍可用；pkg-config 是最好的一把量尺。
if command -v pkg-config >/dev/null 2>&1; then
    if PKG_CONFIG_PATH="$ROOT/lib/pkgconfig" pkg-config --libs rkvc 2>/dev/null |
        grep -qF -- "-L$ROOT/lib"; then
        ok "pkg-config 反推出包内路径"
    else
        bad "pkg-config 前缀（应指向包内 lib/）"
    fi
else
    skip "pkg-config（未安装）"
fi

# 纯 C 消费者：头文件自洽 + 能链接 + 不填 backend_dirs 也能发现包内插件
# （靠 librkvc.so 自身位置反查 lib/rkvc/backends，与 CLI 的 bin/ 反查同源）。
cc=""
for c in aarch64-linux-gnu-gcc cc gcc gcc-11; do
    if command -v "$c" >/dev/null 2>&1; then
        cc="$c"
        break
    fi
done
if [[ -n "$cc" ]]; then
    cat >"$tmp/sdk_probe.c" <<'PROBE_EOF'
/* SDK 面冒烟：零配置发现靠 librkvc.so 自己的位置。 */
#include <stdio.h>

#include "rkvc/rkvc.h"

int main(void) {
    rkvc_context_options opts;
    rkvc_context_options_init(&opts, sizeof(opts)); /* 不填 backend_dirs */
    rkvc_context *ctx = NULL;
    if (rkvc_context_create(&opts, &ctx) != RKVC_OK) {
        fprintf(stderr, "sdk_probe: context create failed\n");
        return 2;
    }

    rkvc_session_request req;
    rkvc_session_request_init(&req, sizeof(req));
    req.operation = RKVC_OP_ENCODE;
    req.codec = RKVC_CODEC_AV1; /* 软编，不依赖硬件节点 */
    req.input.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.input.fmt = RKVC_FRAME_FMT_NV12;
    req.input.width = 640;
    req.input.height = 368;
    req.output.kind = RKVC_ENDPOINT_FRAME_SINK;
    req.output.fmt = RKVC_FRAME_FMT_BITSTREAM;

    rkvc_session *s = NULL;
    rkvc_diagnostic *diag = NULL;
    rkvc_status st = rkvc_session_create(ctx, &req, &s, &diag);
    if (st != RKVC_OK) {
        char buf[1024];
        fprintf(stderr, "sdk_probe: session create failed: %s\n",
                rkvc_status_str(st));
        if (diag) {
            rkvc_diag_fmt_text(diag, buf, sizeof(buf));
            fputs(buf, stderr);
            rkvc_diag_release(diag);
        }
        return 3;
    }
    rkvc_session_destroy(s);
    rkvc_context_destroy(ctx);
    puts("sdk_probe: ok");
    return 0;
}
PROBE_EOF
    if "$cc" -std=c99 -Wall -I"$ROOT/include" "$tmp/sdk_probe.c" \
        -L"$ROOT/lib" -Wl,-rpath,"$ROOT/lib" -lrkvc \
        -o "$tmp/sdk_probe" 2>"$tmp/sdk_probe.log"; then
        ok "C 消费者编译链接（$cc）"
        if [[ ! -f "$BACKENDS/rkvc_av1.so" ]]; then
            skip "SDK 零配置发现（未打包 av1 插件）"
        elif "${RUNNER[@]}" "$tmp/sdk_probe" >/dev/null 2>&1; then
            ok "SDK 零配置发现并建会话"
        else
            bad "SDK 零配置发现"
        fi
    else
        bad "C 消费者编译链接（$cc）"
        sed -n '1,5p' "$tmp/sdk_probe.log"
    fi
else
    skip "SDK 消费者编译（无 C 编译器）"
fi

echo "== av1 软编码冒烟（无硬件依赖；顺带验证插件自动发现）=="
if [[ -f "$BACKENDS/rkvc_av1.so" ]]; then
    head -c $((3 * 640 * 360 * 3 / 2)) /dev/urandom >"$tmp/in.nv12"
    if run encode --codec av1 --input "$tmp/in.nv12" --width 640 \
        --height 360 --pixfmt nv12 --output "$tmp/out.obu" \
        --qp 32 --gop 64 --fps 30 >/dev/null 2>&1; then
        ok "av1 编码 3 帧"
        if [[ -s "$tmp/out.obu" ]]; then
            ok "av1 码流非空（$(stat -c %s "$tmp/out.obu") B）"
        else
            bad "av1 码流非空"
        fi
    else
        bad "av1 编码 3 帧"
    fi
else
    skip "av1（插件未打包）"
fi

echo "== MPP 硬编解码冒烟 =="
if [[ -e /dev/mpp_service ]]; then
    frames=8
    w=640
    h=368
    head -c $((frames * w * h * 3 / 2)) /dev/urandom >"$tmp/hw.nv12"
    if run encode --codec h264 --input "$tmp/hw.nv12" --width $w --height $h \
        --pixfmt nv12 --output "$tmp/out.h264" --qp 26 --gop 30 --fps 30 \
        >/dev/null 2>&1; then
        ok "H264 硬编 $frames 帧"
        if run decode --codec h264 --input "$tmp/out.h264" --width $w \
            --height $h --output "$tmp/back.nv12" >/dev/null 2>&1 &&
            [[ -s "$tmp/back.nv12" ]]; then
            size="$(stat -c %s "$tmp/back.nv12")"
            if ((size % (w * h * 3 / 2) == 0)); then
                ok "H264 回读 $((size / (w * h * 3 / 2))) 帧"
            else
                bad "H264 回读大小 $size 未按帧对齐"
            fi
        else
            bad "H264 硬解"
        fi
    else
        bad "H264 硬编"
    fi
else
    skip "MPP（无 /dev/mpp_service）"
fi

echo "== 模型 =="
mapfile -t models < <(ls "$ROOT"/models/*.rkmodel 2>/dev/null)
if ((${#models[@]})); then
    grep_out "inspect models 列出 ${#models[@]} 个" "\.rkmodel" run inspect \
        models --model-dir "$ROOT/models"
else
    skip "模型（包内无 .rkmodel）"
fi

echo
printf '自测结果: %d 通过, %d 失败, %d 跳过\n' "$pass" "$fail" "$skipped"
if ((fail)); then
    exit 1
fi
exit 0
