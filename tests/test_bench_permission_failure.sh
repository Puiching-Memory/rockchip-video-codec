#!/bin/bash
set -euo pipefail

BUILD_DIR="${1:?usage: $0 <build-dir>}"
ENC="$BUILD_DIR/rkvc_encode"
TMPDIR="$(mktemp -d)"
trap 'rm -rf "$TMPDIR"' EXIT

if [ ! -x "$ENC" ]; then
    echo "FAIL: missing $ENC"
    exit 1
fi

if ! grep -q 'RKVC_ENABLE_FAULT_INJECTION:BOOL=ON' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
    echo "skip bench permission: RKVC_ENABLE_FAULT_INJECTION=OFF"
    exit 77
fi

mkdir -p "$TMPDIR/dev/dma_heap"
touch "$TMPDIR/dev/mpp_service"
touch "$TMPDIR/dev/dma_heap/system-uncached"

set +e
output=$(RKVC_TEST_DEV_ROOT="$TMPDIR" RKVC_TEST_DENY_DEV_PATH="/dev/dma_heap/system-uncached" \
    "$ENC" -i /dev/null -o "$TMPDIR/out.mp4" -s 64x64 -p realtime 2>&1)
status=$?
set -e

if [ "$status" -eq 0 ]; then
    echo "FAIL bench permission: command unexpectedly succeeded"
    printf '%s\n' "$output"
    exit 1
fi

if ! printf '%s\n' "$output" | grep -qi "permission"; then
    echo "FAIL bench permission: missing permission error"
    printf '%s\n' "$output"
    exit 1
fi

echo "session permission failure propagation test passed"
