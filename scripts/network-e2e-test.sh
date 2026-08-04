#!/bin/bash
# network-e2e-test.sh - UDP/RTP 本机回环冒烟（可移植包或构建树）

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

# 输出到非 TTY（日志重定向）或设置 NO_COLOR 时禁用颜色，便于机器解析
if [ ! -t 1 ] || [ -n "${NO_COLOR:-}" ]; then
    RED=''
    GREEN=''
    YELLOW=''
    NC=''
fi

PASS=0
FAIL=0

pass() { PASS=$((PASS+1)); echo -e "${GREEN}✓${NC} $1"; }
fail() { FAIL=$((FAIL+1)); echo -e "${RED}✗${NC} $1"; }
warn() { echo -e "${YELLOW}!${NC} $1"; }

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=portable-test-helpers.sh
if [ -f "$SCRIPT_DIR/portable-test-helpers.sh" ]; then
    # shellcheck disable=SC1091
    source "$SCRIPT_DIR/portable-test-helpers.sh"
fi

find_loopback() {
    if [ -x "$1/examples/bin/example_net_loopback" ]; then
        echo "$1/examples/bin/example_net_loopback"
    elif [ -x "$1/example_net_loopback" ]; then
        echo "$1/example_net_loopback"
    elif [ -x "$SCRIPT_DIR/../.build/release/example_net_loopback" ]; then
        echo "$SCRIPT_DIR/../.build/release/example_net_loopback"
    else
        echo ""
    fi
}

PKG_DIR=""
if [[ $# -gt 0 && -d "$1" ]]; then
    PKG_DIR="$(cd "$1" && pwd)"
    shift
elif [[ -x "$SCRIPT_DIR/bin/rkvc_encode" && -d "$SCRIPT_DIR/lib" ]]; then
    PKG_DIR="$SCRIPT_DIR"
fi

LOOP=""
if [ -n "$PKG_DIR" ]; then
    LOOP="$(find_loopback "$PKG_DIR")"
    export LD_LIBRARY_PATH="${PKG_DIR}/lib:${LD_LIBRARY_PATH:-}"
else
    LOOP="$(find_loopback "$SCRIPT_DIR")"
fi

if [ -z "$LOOP" ] || [ ! -x "$LOOP" ]; then
    echo "错误: 未找到 example_net_loopback"
    exit 2
fi

echo "=== network-e2e: $LOOP ==="

set +e
out_udp=$("$LOOP" udp 19201 2>&1)
st_udp=$?
set -e
if [ "$st_udp" -eq 0 ]; then
    pass "UDP 本机回环"
else
    fail "UDP 本机回环 (exit=$st_udp)"
    printf '%s\n' "$out_udp" | sed 's/^/  | /'
fi

set +e
out_rtp=$("$LOOP" rtp 19202 2>&1)
st_rtp=$?
set -e
if [ "$st_rtp" -eq 0 ]; then
    pass "RTP 本机回环"
else
    fail "RTP 本机回环 (exit=$st_rtp)"
    printf '%s\n' "$out_rtp" | sed 's/^/  | /'
fi

echo "结果: $PASS 通过, $FAIL 失败"
[ "$FAIL" -eq 0 ]
