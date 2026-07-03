#!/bin/bash
# network-e2e-test.sh - v2 可移植包网络相关冒烟测试
#
# v2 的 LiveCapture / UDP-RTP 回环尚在接入中；当前脚本验证：
#   1. 可生成短测试码流
#   2. stream_device_pair 示例可加载并输出 v2 占位信息

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

usage() {
    cat <<'EOF'
用法:
  ./network-e2e-test.sh [选项]
  ./network-e2e-test.sh <portable-package-dir> [选项]

选项:
  -s, --size WxH            测试分辨率 (默认 640x480)
  -n, --frames N            测试帧数 (默认 10)
  -b, --bitrate BPS         编码码率 (默认 1000000)
  -t, --timeout SEC         单步超时时间 (默认 30)
  --keep-tmp                保留临时目录
  -h, --help                显示帮助
EOF
}

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

capture_command() {
    local __status_var="$1"
    local __out_var="$2"
    shift 2

    local cmd_output cmd_status
    set +e
    cmd_output=$("$@" 2>&1)
    cmd_status=$?
    set -e
    printf -v "$__status_var" '%s' "$cmd_status"
    printf -v "$__out_var" '%s' "$cmd_output"
}

file_size() {
    stat -c '%s' "$1" 2>/dev/null || wc -c < "$1"
}

run_with_timeout() {
    local __status_var="$1"
    local __out_var="$2"
    shift 2

    if command -v timeout >/dev/null 2>&1; then
        capture_command "$__status_var" "$__out_var" timeout "$TIMEOUT" "$@"
    else
        warn "系统缺少 timeout，命令不会自动超时"
        capture_command "$__status_var" "$__out_var" "$@"
    fi
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=portable-test-helpers.sh
source "$SCRIPT_DIR/portable-test-helpers.sh"

SIZE="${RKVC_NET_SIZE:-640x480}"
FRAMES="${RKVC_NET_FRAMES:-10}"
BITRATE="${RKVC_NET_BITRATE:-1000000}"
TIMEOUT="${RKVC_NET_TIMEOUT:-30}"
KEEP_TMP="${RKVC_NET_KEEP_TMP:-0}"

if [[ $# -gt 0 && -d "$1" && -x "$1/bin/rkvc_encode" ]]; then
    PKG_DIR="$(cd "$1" && pwd)"
    shift
elif [[ -x "$SCRIPT_DIR/bin/rkvc_encode" && -d "$SCRIPT_DIR/lib" ]]; then
    PKG_DIR="$SCRIPT_DIR"
else
    echo "错误: 未找到可移植包目录"
    usage
    exit 2
fi

while [[ $# -gt 0 ]]; do
    case "$1" in
        -s|--size)
            if [[ -z "${2:-}" ]]; then echo "错误: $1 需要参数"; exit 2; fi
            SIZE="$2"; shift 2 ;;
        -n|--frames)
            if [[ -z "${2:-}" ]]; then echo "错误: $1 需要参数"; exit 2; fi
            FRAMES="$2"; shift 2 ;;
        -b|--bitrate)
            if [[ -z "${2:-}" ]]; then echo "错误: $1 需要参数"; exit 2; fi
            BITRATE="$2"; shift 2 ;;
        -t|--timeout)
            if [[ -z "${2:-}" ]]; then echo "错误: $1 需要参数"; exit 2; fi
            TIMEOUT="$2"; shift 2 ;;
        --keep-tmp)
            KEEP_TMP=1; shift ;;
        -h|--help)
            usage; exit 0 ;;
        *)
            echo "错误: 未知参数 $1"
            usage
            exit 2 ;;
    esac
done

if [[ ! "$SIZE" =~ ^[0-9]+x[0-9]+$ ]]; then
    echo "错误: --size 必须是 WxH，例如 640x480"
    exit 2
fi
for value_name in FRAMES BITRATE TIMEOUT; do
    value="${!value_name}"
    if [[ ! "$value" =~ ^[0-9]+$ || "$value" -le 0 ]]; then
        echo "错误: $value_name 必须是正整数"
        exit 2
    fi
done

PAIR="$PKG_DIR/examples/bin/example_stream_device_pair"
RUN_ENV=(env "LD_LIBRARY_PATH=$PKG_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}")
TMP_ROOT="${TMPDIR:-/tmp}"
WORK_DIR="$(mktemp -d "$TMP_ROOT/rkvc-net-e2e.XXXXXX")"
INPUT="$WORK_DIR/input.mp4"

cleanup() {
    if [[ "$KEEP_TMP" == "1" ]]; then
        echo "保留临时目录: $WORK_DIR"
    else
        rm -rf "$WORK_DIR"
    fi
}
trap cleanup EXIT

echo "=== v2 网络相关冒烟测试 ==="
echo "包目录: $PKG_DIR"
echo "分辨率: $SIZE"
echo "帧数:   $FRAMES"
echo ""

echo "--- 生成测试码流 ---"
if portable_skip_hardware_tests; then
    warn "跳过测试码流生成 (无 RKMPP 设备)"
else
capture_command enc_status enc_out encode_test_clip "$PKG_DIR" "$INPUT" "$SIZE" "$FRAMES" "$BITRATE"
if [ "$enc_status" -eq 0 ] && [ -f "$INPUT" ] && [ "$(file_size "$INPUT")" -gt 0 ]; then
    pass "测试码流生成成功: $(file_size "$INPUT") bytes"
else
    fail "测试码流生成失败 (exit=$enc_status)"
    show_output "encode_test_clip" "$enc_out"
fi
fi

if [ ! -x "$PAIR" ]; then
    fail "缺少示例程序 $PAIR"
else
    run_with_timeout pair_status pair_out "${RUN_ENV[@]}" "$PAIR"
    show_output "example_stream_device_pair" "$pair_out"
    if [ "$pair_status" -eq 0 ] && echo "$pair_out" | grep -q "stream_device_pair"; then
        pass "stream_device_pair 示例可执行"
    else
        fail "stream_device_pair 示例失败 (exit=$pair_status)"
    fi
fi

warn "v2 LiveCapture UDP/RTP 回环尚未接入，完整网络端到端测试将在后续版本恢复"

echo ""
echo "========================================="
echo -e "通过: ${GREEN}${PASS}${NC}  失败: ${RED}${FAIL}${NC}"
if [ "$FAIL" -eq 0 ]; then
    echo -e "${GREEN}网络端到端测试通过${NC}"
    exit 0
else
    echo -e "${RED}网络端到端测试失败${NC}"
    exit 1
fi
