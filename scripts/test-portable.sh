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
#   4. CLI 功能与 JSON 字段、三策略 rkvc_bench
#   5. 编码、解码、转码、后处理上采样、网络冒烟（network-e2e-test.sh）
#   6. 开发头文件与 pkg-config
#   7. CLI 参数错误与包结构负向测试

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0
FAIL=0

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
        elif echo "$output" | grep -q "/root/rk3588-ai-video-codec"; then
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
for name in libavcodec libavformat libavutil libswscale libSvtAv1Enc; do
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
echo ""

# 2. 动态库依赖
echo "--- 动态库依赖 ---"
check_bundled_libs() {
    local bin="$1"
    local name="$2"
    local ldd_output="$3"

    for lib in librkvc libavcodec libavformat libavutil libswscale libSvtAv1Enc librockchip_mpp librknnrt; do
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
echo ""

# 4. 功能测试
echo "--- 功能测试 ---"
capture_run norpath_status norpath_output env -u LD_LIBRARY_PATH "$PKG_DIR/bin/rkvc_info" --version
if [ "$norpath_status" -eq 0 ] && echo "$norpath_output" | grep -q "^rkvc "; then
    pass "无 LD_LIBRARY_PATH: rkvc_info --version"
else
    fail "无 LD_LIBRARY_PATH 运行失败 (exit=$norpath_status)"
    show_output "env -u LD_LIBRARY_PATH rkvc_info --version" "$norpath_output"
fi

capture_run ver_status ver_output env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_info" --version
if [ "$ver_status" -eq 0 ] && echo "$ver_output" | grep -q "^rkvc "; then
    pass "rkvc_info --version: $ver_output"
else
    fail "rkvc_info --version 输出异常 (exit=$ver_status)"
    show_output "rkvc_info --version" "$ver_output"
fi

capture_run json_status json_output env LD_LIBRARY_PATH="$PKG_DIR/lib" "$PKG_DIR/bin/rkvc_info" --json
if [ "$json_status" -eq 0 ] && echo "$json_output" | grep -q '"version"'; then
    pass "rkvc_info --json: 合法输出"
else
    fail "rkvc_info --json 输出异常 (exit=$json_status)"
    show_output "rkvc_info --json" "$json_output"
fi
for field in version h264_enc hevc_enc av1_enc h264_dec hevc_dec av1_dec dma_heap rga max_width max_height; do
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

if portable_skip_hardware_tests; then
    warn "跳过编解码/转码/bench/上采样/网络冒烟 (无 RKMPP 设备；实机设 RKVC_RUN_HARDWARE_TESTS=1 强制)"
    generate_raw_nv12 "$TMPDIR/raw.nv12" 640 480 10
else
if ! portable_mpp_device_accessible; then
    fail "RKVC_RUN_HARDWARE_TESTS=1 但无可用 RKMPP 设备节点"
fi

capture_run enc_status enc_out encode_test_clip "$PKG_DIR" "$TMPDIR/test.mp4" 640x480 10 1000000
if [ "$enc_status" -eq 0 ] && [ -f "$TMPDIR/test.mp4" ]; then
    enc_size=$(file_size "$TMPDIR/test.mp4")
    if [ "$enc_size" -gt 0 ]; then
        pass "编码测试: ${enc_size} bytes"
    else
        fail "编码产物为空: $TMPDIR/test.mp4"
    fi
else
    fail "编码测试失败 (exit=$enc_status)"
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
    fail "跳过解码测试 (编码产物不存在)"
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
       ! echo "$bench_out" | grep -q '\-1\.0 fps'; then
        bench_ok=1
    fi
    if [ "$bench_ok" -eq 1 ]; then
        pass "rkvc_bench session E2E 三策略短测"
    else
        fail "rkvc_bench session E2E 三策略短测失败 (exit=$bench_status)"
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
chmod -x "$NEG_PKG/bin/rkvc_encode"
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

if command -v patchelf >/dev/null 2>&1; then
    copy_package_tree "$NEG_PKG"
    patchelf --set-rpath "$NEG_PKG/lib" "$NEG_PKG/bin/rkvc_info"
    expect_runpath_check_fail "负向包: 可检测绝对 RPATH 注入" \
        "$NEG_PKG/bin/rkvc_info" "negative/bin/rkvc_info" '$ORIGIN/../lib'
else
    warn "跳过绝对 RPATH 注入负向测试 (缺少 patchelf)"
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
