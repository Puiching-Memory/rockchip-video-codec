#!/usr/bin/env bash
# NPU / rkvc_sr 硬件超分推广门禁（RK3588 实机）
#
# 用法:
#   ./scripts/test-npu-sr.sh
#   RKVC_SR_MODEL=/path/to/model.rknn ./scripts/test-npu-sr.sh
#
# 通过标准:
#   - NPU 可访问（/sys/kernel/debug/rknpu/version 或 /dev/dri/by-path/*npu-render*）
#   - Phase-RLFN 模型 models/rkvc-sr/phase_rlfn_sr_x3.rknn 存在
#   - test_session_encode_decode_upscale_3x_ai_sr 通过
#   - 可选：rkvc_session_upscale --post-upscale rkvc_sr 短 smoke
#
# 测试二进制默认在 .build/tests/；CLI smoke 从 .build/release/ 或 .build/portable/ 查找。

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh" 2>/dev/null || true
rkvc_limit_build_jobs 2>/dev/null || true
# shellcheck source=portable-test-helpers.sh
source "$SCRIPT_DIR/portable-test-helpers.sh"

TESTS_DIR="${RKVC_BUILD_DIR:-$ROOT_DIR/.build/tests}"
RELEASE_DIR="$ROOT_DIR/.build/release"
MODEL="${RKVC_SR_MODEL:-}"
if [[ -z "$MODEL" ]]; then
    compat="$(tr -d '\0' </proc/device-tree/compatible 2>/dev/null || true)"
    for soc in rk3588 rk3576 rv1126b; do
        if [[ "$compat" == *"$soc"* &&
              -f "$ROOT_DIR/.build/models/$soc/rkvc-sr/phase_rlfn_sr_x3.rknn" ]]; then
            MODEL="$ROOT_DIR/.build/models/$soc/rkvc-sr/phase_rlfn_sr_x3.rknn"
            break
        fi
    done
    [[ -n "$MODEL" ]] || MODEL="$ROOT_DIR/models/rkvc-sr/phase_rlfn_sr_x3.rknn"
fi

if ! portable_npu_accessible; then
    echo "[error] NPU 不可访问（需 /sys/kernel/debug/rknpu/version 或 /dev/dri/by-path/*npu-render*）" >&2
    exit 1
fi

if [[ ! -f "$MODEL" ]]; then
    echo "[error] 超分模型不存在: $MODEL" >&2
    echo "  运行 scripts/build-models.sh --platform <soc>，或设置 RKVC_SR_MODEL" >&2
    exit 1
fi

echo "[info] NPU: accessible"
if [[ -r /sys/kernel/debug/rknpu/version ]]; then
    echo "[info] driver: $(tr -d '\n' </sys/kernel/debug/rknpu/version)"
fi
echo "[info] model: $MODEL"
echo "[info] tests dir: $TESTS_DIR"

ensure_test_hardware() {
    echo "[info] ensuring test_hardware in $TESTS_DIR ..."
    if [[ ! -f "$TESTS_DIR/build.ninja" && ! -f "$TESTS_DIR/Makefile" ]]; then
        cmake -S "$ROOT_DIR" -B "$TESTS_DIR" -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DRKVC_BUILD_TESTS=ON \
            -DRKVC_BUILD_EXAMPLES=OFF \
            -DRKVC_BUILD_CLI=OFF \
            -DRKVC_ENABLE_FAULT_INJECTION=ON
    fi
    cmake --build "$TESTS_DIR" --target test_hardware -j"${BUILD_JOBS:-$(rkvc_default_build_jobs 2>/dev/null || echo 1)}"
}

ensure_test_hardware

export RKVC_RUN_HARDWARE_TESTS=1
export RKVC_SOURCE_ROOT="$ROOT_DIR"
export RKVC_SR_MODEL="$MODEL"

# test_* 直接运行需补回依赖库路径：DT_RUNPATH 不解析传递依赖
#（libavcodec.so → libSvtAv1Enc.so.4）；ctest 自行注入故无需此处。
if command -v rkvc_dep_library_path >/dev/null 2>&1; then
    export LD_LIBRARY_PATH="$(rkvc_dep_library_path)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
fi

echo "[run] test_session_encode_decode_upscale_3x_ai_sr"
"$TESTS_DIR/test_hardware" test_session_encode_decode_upscale_3x_ai_sr

if command -v ctest >/dev/null 2>&1 && [[ -f "$TESTS_DIR/CTestTestfile.cmake" ]]; then
    echo "[run] ctest -R test_session_encode_decode_upscale_3x_ai_sr"
    # CMake < 3.20 无 --test-dir，进入构建目录执行
    (cd "$TESTS_DIR" && ctest -j1 \
        -R '^test_session_encode_decode_upscale_3x_ai_sr$' --output-on-failure)
fi

# 优先可移植包（自带 RPATH，取版本号最大者）；其次 .build/release
DIST_BIN=""
while IFS= read -r d; do
    if [[ -x "$d/bin/rkvc_session_upscale" && -x "$d/bin/rkvc_encode" ]]; then
        DIST_BIN="$d"
        break
    fi
done < <(ls -1d "$ROOT_DIR"/.build/dist/rkvc-*-linux-*-portable 2>/dev/null | sort -V -r)

UPSCALE_BIN=""
ENC_BIN=""
CLI_LIB_PATH=""
if [[ -n "$DIST_BIN" ]]; then
    UPSCALE_BIN="$DIST_BIN/bin/rkvc_session_upscale"
    ENC_BIN="$DIST_BIN/bin/rkvc_encode"
    CLI_LIB_PATH="$DIST_BIN/lib"
elif [[ -x "$RELEASE_DIR/rkvc_session_upscale" && -x "$RELEASE_DIR/rkvc_encode" ]]; then
    UPSCALE_BIN="$RELEASE_DIR/rkvc_session_upscale"
    ENC_BIN="$RELEASE_DIR/rkvc_encode"
    CLI_LIB_PATH="$RELEASE_DIR:$(rkvc_dep_library_path)"
fi

if [[ -n "$UPSCALE_BIN" && -n "$ENC_BIN" ]]; then
    TMPDIR=$(mktemp -d /tmp/rkvc-npu-sr.XXXXXX)
    trap 'rm -rf "$TMPDIR"' EXIT
    RAW="$TMPDIR/in.nv12"
    ENC="$TMPDIR/in.mp4"
    OUT="$TMPDIR/out.nv12"
    generate_raw_nv12 "$RAW" 1920 1080 2
    echo "[run] rkvc_session_upscale smoke (rkvc_sr) via $ENC_BIN"
    if env LD_LIBRARY_PATH="${CLI_LIB_PATH}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
         "$ENC_BIN" -i "$RAW" -o "$ENC" -s 1920x1080 -p realtime --enc-scale-denom 3 && \
       env LD_LIBRARY_PATH="${CLI_LIB_PATH}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
         "$UPSCALE_BIN" -i "$ENC" -o "$OUT" \
            --width 1920 --height 1080 --enc-scale-denom 3 \
            --post-upscale rkvc_sr --rkvc-sr-model "$MODEL"; then
        out_sz=$(wc -c <"$OUT" || echo 0)
        need=$((1920 * 1080 * 3 / 2))
        if [[ "$out_sz" -lt "$need" ]]; then
            echo "[error] rkvc_session_upscale 输出过小: $out_sz < $need" >&2
            exit 1
        fi
        echo "[ok] session_upscale smoke size=$out_sz"
    else
        echo "[error] rkvc_session_upscale smoke 失败" >&2
        exit 1
    fi
else
    echo "[warn] 跳过 session_upscale smoke（未找到 rkvc_encode / rkvc_session_upscale）"
fi

echo "[ok] NPU / rkvc_sr gate passed"
