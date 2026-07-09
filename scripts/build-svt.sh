#!/bin/bash
# scripts/build-svt.sh — 从 third_party/SVT-AV1 子模块构建 libSvtAv1Enc
#
# 用法: ./scripts/build-svt.sh [--clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SVT_SRC="$PROJECT_DIR/third_party/SVT-AV1"
SVT_BUILD="$PROJECT_DIR/.build/deps/svt-av1-build"
SVT_PREFIX="$PROJECT_DIR/.build/deps/svt-av1-install"

CLEAN=0
[[ "${1:-}" == "--clean" ]] && CLEAN=1

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$SVT_BUILD" "$SVT_PREFIX"
fi

if [[ $CLEAN -eq 0 ]] && \
   { [[ -f "$SVT_PREFIX/lib/libSvtAv1Enc.so" ]] || [[ -f "$SVT_PREFIX/lib/libSvtAv1Enc.a" ]]; } && \
   [[ -x "$SVT_PREFIX/bin/SvtAv1EncApp" ]]; then
    echo "--- SVT-AV1 已构建: $SVT_PREFIX (用 --clean 重建) ---"
    exit 0
fi

if [[ ! -f "$SVT_SRC/CMakeLists.txt" ]]; then
    echo "错误: 子模块未初始化，运行:"
    echo "  git submodule update --init --depth 1 third_party/SVT-AV1"
    exit 1
fi

echo "=== 构建 SVT-AV1 (submodule) ==="
cmake -S "$SVT_SRC" -B "$SVT_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$SVT_PREFIX" \
    -DCMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
    -DBUILD_TESTING=OFF \
    -DBUILD_APPS=ON

cmake --build "$SVT_BUILD" -j"$BUILD_JOBS"
cmake --install "$SVT_BUILD"
echo "--- 完成: $SVT_PREFIX ---"
