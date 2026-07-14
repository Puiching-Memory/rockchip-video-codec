#!/usr/bin/env bash
# RGA 硬件缩放推广门禁（RK3588 实机）
#
# 用法:
#   ./scripts/test-rga.sh
#   RKVC_RGA_SOAK_FRAMES=500 ./scripts/test-rga.sh
#
# 通过标准:
#   - /dev/rga 可访问
#   - test_scale 全部硬件用例通过（含 1080p↔360p bench 路径）
#   - 可选 soak：连续多帧上采样无挂死

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh" 2>/dev/null || true
rkvc_limit_build_jobs 2>/dev/null || true

TESTS_DIR="${RKVC_BUILD_DIR:-$ROOT_DIR/.build/tests}"
SOAK_FRAMES="${RKVC_RGA_SOAK_FRAMES:-200}"

if [[ ! -e /dev/rga ]]; then
    echo "[error] /dev/rga 不存在，跳过 RGA 门禁" >&2
    exit 1
fi

if [[ ! -r /dev/rga || ! -w /dev/rga ]]; then
    echo "[error] /dev/rga 权限不足 (需要读写)" >&2
    exit 1
fi

echo "[info] RGA device: $(ls -la /dev/rga)"
echo "[info] tests dir: $TESTS_DIR"
echo "[info] soak frames: $SOAK_FRAMES"

# 测试二进制在 .build/tests/；勿误用 .build/release 里可能过期的 test_*。
echo "[info] ensuring test_scale in $TESTS_DIR ..."
if [[ ! -f "$TESTS_DIR/build.ninja" && ! -f "$TESTS_DIR/Makefile" ]]; then
    cmake -S "$ROOT_DIR" -B "$TESTS_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DRKVC_BUILD_TESTS=ON \
        -DRKVC_BUILD_EXAMPLES=OFF \
        -DRKVC_BUILD_CLI=OFF \
        -DRKVC_ENABLE_FAULT_INJECTION=ON
fi
cmake --build "$TESTS_DIR" --target test_scale -j"${BUILD_JOBS:-4}"

export RKVC_RUN_HARDWARE_TESTS=1
export RKVC_RGA_SOAK_FRAMES="$SOAK_FRAMES"

# test_* 直接运行需补回依赖库路径：DT_RUNPATH 不解析传递依赖
#（libavcodec.so → libSvtAv1Enc.so.4）；ctest 自行注入故无需此处。
if command -v rkvc_dep_library_path >/dev/null 2>&1; then
    export LD_LIBRARY_PATH="$(rkvc_dep_library_path)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

echo "[run] test_scale (hardware + soak=$SOAK_FRAMES)"
"$TESTS_DIR/test_scale"

if command -v ctest >/dev/null 2>&1 && [[ -f "$TESTS_DIR/CTestTestfile.cmake" ]]; then
    echo "[run] ctest -R test_scale"
    (cd "$TESTS_DIR" && ctest -j1 -R '^test_scale$' --output-on-failure)
fi

if [[ -n "${RKVC_TEST_RAW_NV12:-}" && -r "${RKVC_TEST_RAW_NV12}" && -x "$TESTS_DIR/test_hardware" ]]; then
    echo "[run] test_session_encode_decode_upscale_3x (raw=$RKVC_TEST_RAW_NV12)"
    "$TESTS_DIR/test_hardware" test_session_encode_decode_upscale_3x
fi

echo "[ok] RGA gate passed"
