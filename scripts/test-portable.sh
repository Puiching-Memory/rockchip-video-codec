#!/bin/bash
# scripts/test-portable.sh — 测试可移植包
#
# 用法:
#   ./scripts/test-portable.sh <portable-package-dir>
#   ./test.sh                         # 在可移植包目录内一键自测
#
# 测试项目:
#   1. 文件完整性（含 rkvc_session_upscale / rkvc_yuv_upscale）
#   2. 动态库依赖与包内库来源
#   3. RPATH/RUNPATH 自包含
#   4. CLI 功能与 JSON 字段、四策略 rkvc_bench
#   5. 编码、解码、转码、后处理上采样、网络冒烟（network-e2e-test.sh）
#   6. 开发头文件与 pkg-config
#   7. CLI 参数错误与包结构负向测试
#   8. 强制授权版 license 校验（如存在 *-licensed 目录）
#   9. 跨目录可移植性（复制到临时路径后无 LD_LIBRARY_PATH 运行）

set -euo pipefail
# 统一 umask：非标准加固值（如 0077）会让裸 chmod 符号模式（chmod -x）失败
umask 022

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0
FAIL=0

# 输出到非 TTY（日志重定向）或设置 NO_COLOR 时禁用颜色，便于机器解析
if [ ! -t 1 ] || [ -n "${NO_COLOR:-}" ]; then
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
fi

pass() { PASS=$((PASS+1)); echo -e "${GREEN}✓${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "${RED}✗${NC} $1"; }
warn() { echo -e "${YELLOW}!${NC} $1"; }
show_output() {
    local title="$1"
    local output="$2"

    echo "  --- $title 输出 ---"
    if [ -n "$output" ]; then
        printf '%s\n' "$output" | sed 's/^/  | /'
    else
        echo "  | <无输出>"
    fi
    echo "  --- end ---"
}
run_capture() {
    local __out_var="$1"
    shift

    local output
    set +e
    output=$("$@" 2>&1)
    local status=$?
    set -e
    printf -v "$__out_var" '%s' "$output"
    return "$status"
}
capture_run() {
    local __status_var="$1"
    local __out_var="$2"
    shift 2

    local status
    if run_capture "$__out_var" "$@"; then
        status=0
    else
        status=$?
    fi
    printf -v "$__status_var" '%s' "$status"
}
extract_frames() {
    local output="$1"
    local marker="$2"
    local line

    while IFS= read -r line; do
        if [[ "$line" == *"$marker"* && "$line" =~ ([0-9]+)[[:space:]]*帧 ]]; then
            printf '%s 帧' "${BASH_REMATCH[1]}"
            return
        fi
    done <<< "$output"

    if [[ "$output" =~ ([0-9]+)[[:space:]]*帧 ]]; then
        printf '%s 帧' "${BASH_REMATCH[1]}"
    else
        printf '未知帧数'
    fi
}
file_size() {
    stat -c '%s' "$1" 2>/dev/null || wc -c < "$1"
}
check_runpath_contains() {
    local file="$1"
    local label="$2"
    local expected="$3"
    local output

    output=$(readelf -d "$file" 2>&1 || true)
    if echo "$output" | grep -Eq 'RPATH|RUNPATH'; then
        if echo "$output" | grep -Fq "$expected"; then
            pass "$label: RPATH 包含 $expected"
        else
            fail "$label: RPATH 未包含 $expected"
            show_output "readelf -d $label" "$output"
        fi
        if echo "$output" | grep -q "$PKG_DIR"; then
            fail "$label: RPATH 含包目录绝对路径"
            show_output "readelf -d $label" "$output"
        elif echo "$output" | grep -q "/root/rockchip-video-codec"; then
            fail "$label: RPATH 含构建机绝对路径"
            show_output "readelf -d $label" "$output"
        fi
    else
        fail "$label: 缺少 RPATH/RUNPATH"
        show_output "readelf -d $label" "$output"
    fi
}
expect_runpath_check_fail() {
    local label="$1"
    shift

    local before="$FAIL"
    check_runpath_contains "$@"
    if [ "$FAIL" -gt "$before" ]; then
        FAIL="$before"
        pass "$label"
    else
        fail "$label: RPATH 检测未报告失败"
    fi
}
expect_command_fail() {
    local label="$1"
    local pattern="$2"
    shift 2

    local status output
    set +e
    output=$("$@" 2>&1)
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
        fail "$label: 命令意外成功"
        show_output "$label" "$output"
    elif echo "$output" | grep -Eq "$pattern"; then
        pass "$label"
    else
        fail "$label: 输出未匹配 $pattern (exit=$status)"
        show_output "$label" "$output"
    fi
}
copy_package_tree() {
    local dst="$1"

    rm -rf "$dst"
    mkdir -p "$dst"
    (cd "$PKG_DIR" && tar cf - .) | (cd "$dst" && tar xf -)
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=portable-test-helpers.sh
source "$SCRIPT_DIR/portable-test-helpers.sh"

if [[ $# -gt 0 ]]; then
    PKG_DIR="$1"
elif [[ -x "$SCRIPT_DIR/bin/rkvc_info" && -d "$SCRIPT_DIR/lib" ]]; then
    PKG_DIR="$SCRIPT_DIR"
else
    echo "用法: $0 <package-dir>"
    echo "或在可移植包目录内运行: ./test.sh"
    exit 2
fi

PKG_DIR="$(cd "$PKG_DIR" && pwd)"

# licensed 包默认 license 探测失败但同级自测 license 有效时，功能测试统一改用之
if [[ "$PKG_DIR" == *-licensed && -z "${RKVC_LICENSE_FILE:-}" && -f "${PKG_DIR}.lic" ]] \
   && ! "$PKG_DIR/bin/rkvc_info" --version >/dev/null 2>&1 \
   && env RKVC_LICENSE_FILE="${PKG_DIR}.lic" "$PKG_DIR/bin/rkvc_info" --version >/dev/null 2>&1; then
    export RKVC_LICENSE_FILE="${PKG_DIR}.lic"
fi

echo "=== 测试可移植包: $PKG_DIR ==="
echo ""

# 1. 文件完整性
echo "--- 文件完整性 ---"
for f in bin/rkvc_encode bin/rkvc_decode bin/rkvc_info bin/rkvc_bench bin/rkvc_transcode \
         bin/rkvc_session_upscale bin/rkvc_yuv_upscale; do
    if [ -x "$PKG_DIR/$f" ]; then
        pass "存在且可执行: $f"
    elif [ -e "$PKG_DIR/$f" ]; then
        fail "存在但不可执行: $f"
    else
        fail "缺失: $f"
    fi
done
for f in lib/librkvc.so include/rkvc/rkvc.h; do
    if [ -e "$PKG_DIR/$f" ]; then
        pass "存在: $f"
    else
        fail "缺失: $f"
    fi
done
# ffmpeg 库使用通配符匹配 (版本号随 ffmpeg 版本变化)
for name in "${RKVC_BUNDLED_FFMPEG_LIBS[@]}" libSvtAv1Enc; do
    if ls "$PKG_DIR/lib/${name}.so."* >/dev/null 2>&1; then
        pass "存在: lib/${name}.so.*"
    else
        fail "缺失: lib/${name}.so.*"
    fi
done
for name in librockchip_mpp librockchip_vpu; do
    if ls "$PKG_DIR/lib/${name}.so."* >/dev/null 2>&1; then
        pass "存在: lib/${name}.so.*"
    else
        fail "缺失: lib/${name}.so.*"
    fi
done
if [ -x "$PKG_DIR/test.sh" ]; then
    pass "存在且可执行: test.sh"
else
    fail "缺失或不可执行: test.sh"
fi
if [ -x "$PKG_DIR/network-e2e-test.sh" ]; then
    pass "存在且可执行: network-e2e-test.sh"
else
    fail "缺失或不可执行: network-e2e-test.sh"
fi
if [ -f "$PKG_DIR/lib/librknnrt.so" ]; then
    pass "存在: lib/librknnrt.so"
    if [ -f "$PKG_DIR/models/rkvc_sr_x3.crypt.rknn" ]; then
        pass "存在: models/rkvc_sr_x3.crypt.rknn"
    else
        fail "已打包 librknnrt 但缺失: models/rkvc_sr_x3.crypt.rknn"
    fi
    # MLVC bundle 需整包随分发（RKNN + PMF + QP 补丁 + manifest）
    for variant in mlvc mlvc-s; do
        if ls "$PKG_DIR/models/$variant"/MLVCEncoder_*.rknn >/dev/null 2>&1 \
           && ls "$PKG_DIR/models/$variant"/MLVCDecoder_*.rknn >/dev/null 2>&1 \
           && [ -f "$PKG_DIR/models/$variant/gaussian.bin" ] \
           && [ -f "$PKG_DIR/models/$variant/bitest.bin" ] \
           && [ -f "$PKG_DIR/models/$variant/mlvc_rknn_export_manifest.json" ]; then
            pass "存在: models/$variant/ (MLVC bundle)"
        else
            fail "已打包 librknnrt 但缺失 MLVC bundle: models/$variant/"
        fi
    done
else
    warn "未打包 librknnrt.so（本构建可能未启用 RKNN）"
fi
if [ -f "$PKG_DIR/lib/librga.so" ]; then
    pass "存在: lib/librga.so"
else
    fail "缺失: lib/librga.so"
fi
echo ""

# 2. 动态库依赖
echo "--- 动态库依赖 ---"
check_bundled_libs() {
    local bin="$1"
    local name="$2"
    local ldd_output="$3"

    for lib in "${RKVC_BUNDLED_ALL_LIBS[@]}"; do
        if echo "$ldd_output" | grep -q "$lib"; then
            if echo "$ldd_output" | grep "$lib" | grep -vq "$PKG_DIR/lib/"; then
                fail "$name: $lib 未解析到包内 lib/"
                echo "$ldd_output" | grep "$lib" | sed 's/^/  /'
            fi
        fi
    done
}

check_binary_deps() {
    local bin="$1"
    name="$(basename "$bin")"
    ldd_output=$(LD_LIBRARY_PATH="$PKG_DIR/lib" ldd "$bin" 2>&1 || true)
    not_found=$(echo "$ldd_output" | grep "not found" || true)
    if [ -z "$not_found" ]; then
        pass "$name: 所有依赖已满足"
    else
        fail "$name: 缺失依赖"
        echo "  $not_found"
        show_output "ldd $name" "$ldd_output"
    fi
    check_bundled_libs "$bin" "$name" "$ldd_output"
}

for bin in "$PKG_DIR/bin/"*; do
    [ -f "$bin" ] || continue
    check_binary_deps "$bin"
done
for bin in "$PKG_DIR/examples/bin/"*; do
    [ -f "$bin" ] || continue
    check_binary_deps "$bin"
done
echo ""

# 3. RPATH / RUNPATH
echo "--- RPATH / RUNPATH ---"
if ! command -v readelf >/dev/null 2>&1; then
    warn "跳过 RPATH 组 (缺少 readelf，无法解析 ELF 动态段)"
else
for bin in "$PKG_DIR/bin/"*; do
    [ -f "$bin" ] || continue
    check_runpath_contains "$bin" "bin/$(basename "$bin")" '$ORIGIN/../lib'
done
for bin in "$PKG_DIR/examples/bin/"*; do
    [ -f "$bin" ] || continue
    check_runpath_contains "$bin" "examples/bin/$(basename "$bin")" '$ORIGIN/../../lib'
done
for lib in "$PKG_DIR/lib/"*.so.*; do
    [ -f "$lib" ] || continue
    [ -L "$lib" ] && continue
    base="$(basename "$lib")"
    case "$base" in
        libSvtAv1Enc.so.*)
            # 仅依赖 libc，无包内库互引
            if readelf -d "$lib" 2>/dev/null | grep -Eq 'RPATH|RUNPATH'; then
                pass "lib/$base: RPATH 已设置"
            else
                pass "lib/$base: 无包内依赖，跳过 RPATH 检查"
            fi
            continue
            ;;
    esac
    check_runpath_contains "$lib" "lib/$base" '$ORIGIN'
done
fi
echo ""

# 4. 功能测试
echo "--- 功能测试 ---"
# 自包含性验证用 --json：licensed 版 --version 会触发 license 校验，干扰 RPATH 结论
capture_run norpath_status norpath_output env -u LD_LIBRARY_PATH "$PKG_DIR/bin/rkvc_info" --json
if [ "$norpath_status" -eq 0 ] && echo "$norpath_output" | grep -q '"version"'; then
    pass "无 LD_LIBRARY_PATH: rkvc_info --json"
else
    fail "无 LD_LIBRARY_PATH 运行失败 (exit=$norpath_status)"
    show_output "env -u LD_LIBRARY_PATH rkvc_info --json" "$norpath_output"
fi

if portable_license_blocked "$PKG_DIR"; then
    warn "跳过 rkvc_info --version (授权版包无有效 license，--version 触发 license 校验)"
else
capture_run ver_status ver_output env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_info" --version
if [ "$ver_status" -eq 0 ] && echo "$ver_output" | grep -q "^rkvc "; then
    pass "rkvc_info --version: $ver_output"
else
    fail "rkvc_info --version 输出异常 (exit=$ver_status)"
    show_output "rkvc_info --version" "$ver_output"
fi
fi

capture_run json_status json_output env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_info" --json
if [ "$json_status" -eq 0 ] && echo "$json_output" | grep -q '"version"'; then
    pass "rkvc_info --json: 合法输出"
else
    fail "rkvc_info --json 输出异常 (exit=$json_status)"
    show_output "rkvc_info --json" "$json_output"
fi
for field in version soc h264_enc hevc_enc av1_enc h264_dec hevc_dec av1_dec dma_heap rga rknn; do
    if echo "$json_output" | grep -q "\"$field\""; then
        pass "rkvc_info --json 字段: $field"
    else
        fail "rkvc_info --json 缺少字段: $field"
        show_output "rkvc_info --json" "$json_output"
    fi
done

echo ""
echo "--- 编解码测试 ---"
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

if portable_license_blocked "$PKG_DIR"; then
    warn "授权版包且无有效 license：跳过编解码/转码/bench/上采样/网络冒烟功能组"
    warn "  需向厂商申请 license (机器码: $PKG_DIR/bin/rkvc_lic machine-id)，"
    warn "  置于 ${PKG_DIR}.lic 或 ~/.config/rkvc/license.lic，或设 RKVC_LICENSE_FILE"
    generate_raw_nv12 "$TMPDIR/raw.nv12" 640 480 10
elif portable_skip_hardware_tests; then
    warn "跳过编解码/转码/bench/上采样/网络冒烟 (无 RKMPP 设备；实机设 RKVC_RUN_HARDWARE_TESTS=1 强制)"
    generate_raw_nv12 "$TMPDIR/raw.nv12" 640 480 10
else
if ! portable_mpp_device_accessible; then
    fail "RKVC_RUN_HARDWARE_TESTS=1 但无可用 RKMPP 设备节点"
fi

capture_run enc_status enc_out encode_test_clip "$PKG_DIR" "$TMPDIR/test.mp4" 640x480 10 1000000
enc_fail_reason=""
if [ "$enc_status" -eq 0 ] && [ -f "$TMPDIR/test.mp4" ]; then
    enc_size=$(file_size "$TMPDIR/test.mp4")
    if [ "$enc_size" -gt 0 ]; then
        pass "编码测试: ${enc_size} bytes"
    else
        fail "编码产物为空: $TMPDIR/test.mp4"
    fi
else
    enc_fail_reason="编码测试失败 (exit=$enc_status)"
    if printf '%s' "$enc_out" | grep -qi 'license'; then
        enc_fail_reason="$enc_fail_reason: license 未授权"
    fi
    fail "$enc_fail_reason"
    show_output "encode_test_clip" "$enc_out"
fi

if [ -f "$TMPDIR/test.mp4" ]; then
    capture_run dec_status dec_out env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_decode" \
        -i "$TMPDIR/test.mp4" -o "$TMPDIR/decoded.nv12"
    if [ "$dec_status" -eq 0 ] && [ -f "$TMPDIR/decoded.nv12" ]; then
        dec_size=$(file_size "$TMPDIR/decoded.nv12")
        frame_size=$((640 * 480 * 3 / 2))
        if [ "$dec_size" -gt 0 ] && [ $((dec_size % frame_size)) -eq 0 ]; then
            pass "解码测试: ${dec_size} bytes"
        else
            fail "解码产物大小异常: ${dec_size} bytes (frame_size=$frame_size)"
        fi
    else
        fail "rkvc_decode 解码失败 (exit=$dec_status)"
        echo "  命令: $PKG_DIR/bin/rkvc_decode -i $TMPDIR/test.mp4 -o $TMPDIR/decoded.nv12"
        show_output "rkvc_decode" "$dec_out"
    fi
else
    fail "跳过解码测试 (编码产物不存在${enc_fail_reason:+：$enc_fail_reason})"
fi

generate_raw_nv12 "$TMPDIR/raw.nv12" 640 480 10
capture_run cli_enc_status cli_enc_out env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_encode" \
    -i "$TMPDIR/raw.nv12" -o "$TMPDIR/cli.mp4" -s 640x480 -p realtime
if [ "$cli_enc_status" -eq 0 ] && [ -f "$TMPDIR/cli.mp4" ] && [ "$(file_size "$TMPDIR/cli.mp4")" -gt 0 ]; then
    pass "rkvc_encode CLI 原始 NV12 编码"
else
    fail "rkvc_encode CLI 原始 NV12 编码失败 (exit=$cli_enc_status)"
    show_output "rkvc_encode" "$cli_enc_out"
fi

if [ -f "$TMPDIR/test.mp4" ]; then
    capture_run trans_status trans_out env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_transcode" \
        -i "$TMPDIR/test.mp4" -o "$TMPDIR/transcoded.mp4" -p balanced -s 640x480
    if [ "$trans_status" -eq 0 ] && [ -f "$TMPDIR/transcoded.mp4" ] && [ "$(file_size "$TMPDIR/transcoded.mp4")" -gt 0 ]; then
        pass "rkvc_transcode balanced 转码"
    else
        fail "rkvc_transcode 转码失败 (exit=$trans_status)"
        show_output "rkvc_transcode" "$trans_out"
    fi
fi

capture_run net_status net_out env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/network-e2e-test.sh"
if [ "$net_status" -eq 0 ]; then
    pass "网络端到端脚本"
else
    fail "网络端到端脚本失败 (exit=$net_status)"
    show_output "network-e2e-test.sh" "$net_out"
fi

if [ -f "$TMPDIR/test.mp4" ]; then
    capture_run bench_status bench_out env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_bench" \
        -i "$TMPDIR/test.mp4" -o "$TMPDIR/bench" -s 640x480
    bench_ok=0
    if [ "$bench_status" -eq 0 ] && \
       echo "$bench_out" | grep -qE 'REALTIME \(H\.264\)[[:space:]]+[0-9]+\.[0-9]+ fps' && \
       echo "$bench_out" | grep -qE 'BALANCED \(HEVC\)[[:space:]]+[0-9]+\.[0-9]+ fps' && \
       echo "$bench_out" | grep -qE 'QUALITY \(AV1\)[[:space:]]+[0-9]+\.[0-9]+ fps' && \
       echo "$bench_out" | grep -qE 'OFFLINE \(AV1 HQ\)[[:space:]]+[0-9]+\.[0-9]+ fps' && \
       ! echo "$bench_out" | grep -q '\-1\.0 fps'; then
        bench_ok=1
    fi
    if [ "$bench_ok" -eq 1 ]; then
        pass "rkvc_bench session E2E 四策略短测"
    else
        fail "rkvc_bench session E2E 四策略短测失败 (exit=$bench_status)"
        echo "  命令: $PKG_DIR/bin/rkvc_bench -i $TMPDIR/test.mp4 -o $TMPDIR/bench -s 640x480"
        show_output "rkvc_bench" "$bench_out"
    fi
fi

if [ -x "$PKG_DIR/bin/rkvc_session_upscale" ] && [ -f "$TMPDIR/test.mp4" ]; then
    capture_run up_status up_out env LD_LIBRARY_PATH="$PKG_DIR/lib" \
        "$PKG_DIR/bin/rkvc_session_upscale" \
        -i "$TMPDIR/test.mp4" -o "$TMPDIR/upscaled.nv12" \
        --width 640 --height 480 --enc-scale-denom 2 --post-upscale bilinear
    up_frame=$((640 * 480 * 3 / 2))
    up_size=0
    [ -f "$TMPDIR/upscaled.nv12" ] && up_size=$(file_size "$TMPDIR/upscaled.nv12")
    if [ "$up_status" -eq 0 ] && [ "$up_size" -ge "$up_frame" ]; then
        pass "rkvc_session_upscale 2× 后处理上采样"
    else
        fail "rkvc_session_upscale 后处理上采样失败 (exit=$up_status, size=$up_size)"
        show_output "rkvc_session_upscale" "$up_out"
    fi
fi

# NPU / rkvc_sr smoke（需包内模型 + NPU；无 NPU 时默认 skip）
SR_MODEL="$PKG_DIR/models/rkvc_sr_x3.crypt.rknn"
if [ ! -f "$PKG_DIR/lib/librknnrt.so" ] || [ ! -f "$SR_MODEL" ]; then
    warn "跳过 rkvc_sr NPU 冒烟 (包内无 librknnrt 或 models/)"
elif portable_skip_npu_tests; then
    warn "跳过 rkvc_sr NPU 冒烟 (无 NPU；实机设 RKVC_RUN_HARDWARE_TESTS=1 强制)"
elif ! portable_npu_accessible; then
    fail "RKVC_RUN_HARDWARE_TESTS=1 但 NPU 不可访问"
elif [ -x "$PKG_DIR/bin/rkvc_session_upscale" ] && [ -x "$PKG_DIR/bin/rkvc_encode" ]; then
    generate_raw_nv12 "$TMPDIR/sr_raw.nv12" 1920 1080 2
    capture_run sr_enc_status sr_enc_out env LD_LIBRARY_PATH="$PKG_DIR/lib" \
        "$PKG_DIR/bin/rkvc_encode" -i "$TMPDIR/sr_raw.nv12" -o "$TMPDIR/sr_enc.mp4" \
        -s 1920x1080 -p realtime --enc-scale-denom 3
    if [ "$sr_enc_status" -ne 0 ] || [ ! -f "$TMPDIR/sr_enc.mp4" ]; then
        fail "rkvc_sr 前置编码失败 (exit=$sr_enc_status)"
        show_output "rkvc_encode (sr)" "$sr_enc_out"
    else
        capture_run sr_up_status sr_up_out env LD_LIBRARY_PATH="$PKG_DIR/lib" \
            "$PKG_DIR/bin/rkvc_session_upscale" \
            -i "$TMPDIR/sr_enc.mp4" -o "$TMPDIR/sr_out.nv12" \
            --width 1920 --height 1080 --enc-scale-denom 3 \
            --post-upscale rkvc_sr --rkvc-sr-model "$SR_MODEL"
        sr_frame=$((1920 * 1080 * 3 / 2))
        sr_size=0
        [ -f "$TMPDIR/sr_out.nv12" ] && sr_size=$(file_size "$TMPDIR/sr_out.nv12")
        if [ "$sr_up_status" -eq 0 ] && [ "$sr_size" -ge "$sr_frame" ]; then
            pass "rkvc_session_upscale rkvc_sr 3× NPU 冒烟"
        else
            fail "rkvc_session_upscale rkvc_sr 失败 (exit=$sr_up_status, size=$sr_size)"
            show_output "rkvc_session_upscale rkvc_sr" "$sr_up_out"
        fi
    fi
else
    warn "跳过 rkvc_sr NPU 冒烟 (缺少 encode/upscale 工具)"
fi
fi

echo ""
echo "--- 开发文件 ---"
for h in rkvc.h types.h buffer.h pipeline.h policy.h port.h session.h; do
    if [ -f "$PKG_DIR/include/rkvc/$h" ]; then
        pass "头文件: $h"
    else
        fail "缺失头文件: $h"
    fi
done

if [ -f "$PKG_DIR/share/pkgconfig/rkvc.pc" ]; then
    pass "pkg-config: rkvc.pc"
else
    fail "缺失: rkvc.pc"
fi

if command -v pkg-config >/dev/null 2>&1 && command -v cc >/dev/null 2>&1; then
    cat > "$TMPDIR/minimal.c" <<'EOF'
#include <rkvc/rkvc.h>
int main(void) { return rkvc_version() ? 0 : 1; }
EOF
    if PKG_CONFIG_PATH="$PKG_DIR/share/pkgconfig" \
       cc "$TMPDIR/minimal.c" -o "$TMPDIR/minimal" \
          $(PKG_CONFIG_PATH="$PKG_DIR/share/pkgconfig" pkg-config --cflags --libs rkvc) \
          -Wl,-rpath,"$PKG_DIR/lib" 2>"$TMPDIR/minimal-build.log" &&
       "$TMPDIR/minimal" >/dev/null 2>"$TMPDIR/minimal-run.log"; then
        pass "pkg-config 最小程序可编译运行"
    else
        fail "pkg-config 最小程序编译或运行失败"
        show_output "minimal build" "$(cat "$TMPDIR/minimal-build.log" 2>/dev/null || true)"
        show_output "minimal run" "$(cat "$TMPDIR/minimal-run.log" 2>/dev/null || true)"
    fi
else
    warn "跳过 pkg-config 最小编译测试 (缺少 pkg-config 或 cc)"
fi

echo ""
echo "--- 负向测试 ---"
expect_command_fail "rkvc_encode 缺少输出参数" "usage|raw.nv12" \
    env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_encode" -i "$TMPDIR/raw.nv12" -s 640x480
expect_command_fail "rkvc_encode 缺少输入源" "usage|raw.nv12" \
    env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_encode" -o "$TMPDIR/unused.mp4" -s 640x480
expect_command_fail "rkvc_decode 缺少输入文件" "usage|decode|in.mp4" \
    env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_decode" -o "$TMPDIR/unused.nv12"
expect_command_fail "rkvc_decode 输入文件不存在" "not found|No such file|codec or device not found|session|usage|decode" \
    env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_decode" -i "$TMPDIR/missing.mp4" -o "$TMPDIR/unused.nv12"

NEG_PKG="$TMPDIR/package-negative"
copy_package_tree "$NEG_PKG"
chmod a-x "$NEG_PKG/bin/rkvc_encode"
expect_command_fail "负向包: rkvc_encode 不可执行" "Permission denied|权限不够|denied" \
    env LD_LIBRARY_PATH="$NEG_PKG/lib" "$NEG_PKG/bin/rkvc_encode" -h

copy_package_tree "$NEG_PKG"
rm -f "$NEG_PKG/lib/librockchip_mpp.so"*
neg_ldd=$(env -u LD_LIBRARY_PATH ldd "$NEG_PKG/bin/rkvc_info" 2>&1 || true)
neg_mpp_line=$(printf '%s\n' "$neg_ldd" | grep "librockchip_mpp" || true)
if [ -z "$neg_mpp_line" ]; then
    fail "负向包: ldd 未显示 librockchip_mpp 依赖"
    show_output "negative ldd" "$neg_ldd"
elif printf '%s\n' "$neg_mpp_line" | grep -Fq "$NEG_PKG/lib/"; then
    fail "负向包: 删除 librockchip_mpp 后仍解析到包内"
    show_output "negative ldd" "$neg_ldd"
else
    pass "负向包: 可检测 librockchip_mpp 缺失或串入系统库"
fi

if command -v patchelf >/dev/null 2>&1 && command -v readelf >/dev/null 2>&1; then
    copy_package_tree "$NEG_PKG"
    patchelf --set-rpath "$NEG_PKG/lib" "$NEG_PKG/bin/rkvc_info"
    expect_runpath_check_fail "负向包: 可检测绝对 RPATH 注入" \
        "$NEG_PKG/bin/rkvc_info" "negative/bin/rkvc_info" '$ORIGIN/../lib'
else
    warn "跳过绝对 RPATH 注入负向测试 (缺少 patchelf 或 readelf)"
fi

# 跨目录可移植性：将包复制到另一路径后仍能无 LD_LIBRARY_PATH 运行
# 验证 RPATH 使用 $ORIGIN，不依赖原始 PKG_DIR 绝对路径
echo ""
echo "--- 跨目录可移植性 ---"
MOVED_PKG="$TMPDIR/package-moved"
rm -rf "$MOVED_PKG"
mkdir -p "$MOVED_PKG"
(cd "$PKG_DIR" && tar cf - .) | (cd "$MOVED_PKG" && tar xf -)
if [ -x "$MOVED_PKG/bin/rkvc_info" ]; then
    capture_run moved_status moved_output env -u LD_LIBRARY_PATH "$MOVED_PKG/bin/rkvc_info" --json
    if [ "$moved_status" -eq 0 ] && echo "$moved_output" | grep -q '"version"'; then
        pass "跨目录运行: 移动后 rkvc_info --json 成功"
    else
        fail "跨目录运行: 移动后 rkvc_info --json 失败 (exit=$moved_status)"
        show_output "moved rkvc_info --json" "$moved_output"
    fi

    capture_run moved_json_status moved_json_output env -u LD_LIBRARY_PATH "$MOVED_PKG/bin/rkvc_info" --json
    if [ "$moved_json_status" -eq 0 ] && echo "$moved_json_output" | grep -q '"version"'; then
        pass "跨目录运行: 移动后 rkvc_info --json 成功"
    else
        fail "跨目录运行: 移动后 rkvc_info --json 失败 (exit=$moved_json_status)"
        show_output "moved rkvc_info --json" "$moved_json_output"
    fi

    capture_run moved_ldd_status moved_ldd_output env -u LD_LIBRARY_PATH ldd "$MOVED_PKG/bin/rkvc_info"
    if [ "$moved_ldd_status" -eq 0 ] && ! echo "$moved_ldd_output" | grep -q "not found"; then
        pass "跨目录运行: 移动后所有动态依赖已解析"
    else
        fail "跨目录运行: 移动后存在未解析依赖"
        show_output "moved ldd" "$moved_ldd_output"
    fi
else
    fail "跨目录运行: 复制后的包缺少 rkvc_info"
fi

echo ""
echo "--- 强制授权版 license 校验 ---"
LIC_PKG_DIR=""
# 若当前包路径不是 licensed 版，尝试查找同级 *-licensed 目录
if [[ "$PKG_DIR" == *-licensed ]]; then
    LIC_PKG_DIR="$PKG_DIR"
else
    LIC_PKG_DIR="${PKG_DIR}-licensed"
fi

if [ -d "$LIC_PKG_DIR" ] && [ -x "$LIC_PKG_DIR/bin/rkvc_info" ]; then
    echo "  授权版包: $LIC_PKG_DIR"

    # 无 license 时应失败
    capture_run lic_no_status lic_no_output \
        env -u LD_LIBRARY_PATH -u RKVC_LICENSE_FILE "$LIC_PKG_DIR/bin/rkvc_info" --version
    if [ "$lic_no_status" -ne 0 ] && \
       echo "$lic_no_output" | grep -Eiq "license|lic|授权|unauthorized|denied"; then
        pass "license: 无 license 时 rkvc_info 拒绝运行"
    else
        fail "license: 无 license 时 rkvc_info 未正确拒绝 (exit=$lic_no_status)"
        show_output "licensed rkvc_info (no license)" "$lic_no_output"
    fi

    # 使用本机自测 license 时应成功
    LIC_FILE="$LIC_PKG_DIR.lic"
    if [ -f "$LIC_FILE" ]; then
        capture_run lic_ok_status lic_ok_output \
            env -u LD_LIBRARY_PATH RKVC_LICENSE_FILE="$LIC_FILE" "$LIC_PKG_DIR/bin/rkvc_info" --version
        if [ "$lic_ok_status" -eq 0 ] && echo "$lic_ok_output" | grep -q "^rkvc "; then
            pass "license: 有效 license 下 rkvc_info 正常运行"
        else
            fail "license: 有效 license 下 rkvc_info 运行失败 (exit=$lic_ok_status)"
            show_output "licensed rkvc_info (with license)" "$lic_ok_output"
        fi
    else
        warn "未找到本机自测 license: $LIC_FILE，跳过 license 正向测试"
    fi

    # rkvc_lic 工具存在性
    if [ -x "$LIC_PKG_DIR/bin/rkvc_lic" ]; then
        pass "license: 包内包含 rkvc_lic 工具"
    else
        fail "license: 授权版包缺少 rkvc_lic 工具"
    fi
else
    warn "跳过 license 校验 (未找到 *-licensed 包: $LIC_PKG_DIR)"
fi

# 验证分发版 rkvc_lic 仅含机器码采集/校验能力，不含 genkey/issue/inspect。
# 这是安全负向测试：终端客户不应获得私钥签发能力。
if [ -x "$PKG_DIR/bin/rkvc_lic" ]; then
    echo ""
    echo "--- 分发版 rkvc_lic 能力负向测试 ---"

    capture_run lic_genkey_status lic_genkey_output "$PKG_DIR/bin/rkvc_lic" genkey
    if [ "$lic_genkey_status" -ne 0 ] && \
       echo "$lic_genkey_output" | grep -Eiq "usage|用法|machine-id|verify"; then
        pass "分发版 rkvc_lic: genkey 已被禁用"
    else
        fail "分发版 rkvc_lic: genkey 不应可用"
        show_output "rkvc_lic genkey" "$lic_genkey_output"
    fi

    capture_run lic_issue_status lic_issue_output "$PKG_DIR/bin/rkvc_lic" issue
    if [ "$lic_issue_status" -ne 0 ] && \
       echo "$lic_issue_output" | grep -Eiq "usage|用法|machine-id|verify"; then
        pass "分发版 rkvc_lic: issue 已被禁用"
    else
        fail "分发版 rkvc_lic: issue 不应可用"
        show_output "rkvc_lic issue" "$lic_issue_output"
    fi

    capture_run lic_inspect_status lic_inspect_output "$PKG_DIR/bin/rkvc_lic" inspect
    if [ "$lic_inspect_status" -ne 0 ] && \
       echo "$lic_inspect_output" | grep -Eiq "usage|用法|machine-id|verify"; then
        pass "分发版 rkvc_lic: inspect 已被禁用"
    else
        fail "分发版 rkvc_lic: inspect 不应可用"
        show_output "rkvc_lic inspect" "$lic_inspect_output"
    fi

    capture_run lic_machine_status lic_machine_output env -u LD_LIBRARY_PATH "$PKG_DIR/bin/rkvc_lic" machine-id
    # machine-id 诊断在 stderr，stdout 末行为 64 hex；capture_run 合并了二者
    if [ "$lic_machine_status" -eq 0 ] && \
       echo "$lic_machine_output" | grep -Eq '(^|machine_id : )[0-9a-f]{64}$'; then
        pass "分发版 rkvc_lic: machine-id 可用"
    else
        fail "分发版 rkvc_lic: machine-id 不可用 (exit=$lic_machine_status)"
        show_output "rkvc_lic machine-id" "$lic_machine_output"
    fi
fi

# 总结
echo ""
echo "========================================="
echo -e "通过: ${GREEN}${PASS}${NC}  失败: ${RED}${FAIL}${NC}"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}所有测试通过!${NC}"
    exit 0
else
    echo -e "${RED}有测试失败!${NC}"
    exit 1
fi
