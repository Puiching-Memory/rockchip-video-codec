#!/bin/bash
# 交叉构建 aarch64 板件运行时（161/214 共用）并装配部署包。
# 用法: tools/board-build.sh <board>   board = rk3576 | rv1126b
# 产物: .build/board-<board>/deploy/{bin,lib,share} 自包含部署树
set -euo pipefail

BOARD="${1:?用法: board-build.sh <rk3576|rv1126b>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/.build/board-$BOARD"
STAGING="$ROOT/.build/cross-rknn"
DEPLOY="$BUILD/deploy"

# RKNN 运行库 staging（容器 /tmp 会丢，固定在项目 .build 内）
mkdir -p "$STAGING/include" "$STAGING/lib"
if [ ! -f "$STAGING/lib/librknnrt.so" ]; then
    for SRC in /tmp/rknn-staging \
               /root/workspace/bench-refresh-20260907/runtime-libs; do
        if [ -f "$SRC/lib/librknnrt.so" ]; then
            cp "$SRC/lib/librknnrt.so" "$STAGING/lib/"
            cp "$SRC/include/rknn_api.h" "$STAGING/include/"
            break
        fi
    done
fi
[ -f "$STAGING/lib/librknnrt.so" ] || { echo "找不到 aarch64 librknnrt.so"; exit 1; }

cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchains/aarch64-linux-gnu.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DRKVC_BUILD_SHARED=ON -DRKVC_BUILD_STATIC=OFF \
    -DRKVC_BUILD_CLI=ON -DRKVC_BUILD_TESTS=OFF -DRKVC_BUILD_EXAMPLES=OFF \
    -DRKVC_BUILD_DOCS=OFF -DRKVC_BUILD_BACKEND_MLVC=ON \
    -DRKVC_BUILD_BACKEND_MPP=OFF -DRKVC_BUILD_BACKEND_RGA=OFF \
    -DRKVC_BUILD_BACKEND_RKNN=OFF -DRKVC_BUILD_BACKEND_SVT=OFF \
    -DRKVC_BUILD_BACKEND_FFMPEG=OFF \
    -DRKNN_INSTALL_PREFIX="$STAGING" \
    -DCMAKE_INSTALL_PREFIX="/rkvc-$BOARD"
cmake --build "$BUILD" -j8
cmake --install "$BUILD" --prefix "$DEPLOY" >/dev/null

# 部署树：librknnrt 与后端 DSO 同目录布局
cp "$STAGING/lib/librknnrt.so" "$DEPLOY/lib/"
if [ ! -f "$DEPLOY/lib/rkvc/backends/rkvc_backend_mlvc.so" ]; then
    mkdir -p "$DEPLOY/lib/rkvc/backends"
    find "$BUILD" -name "rkvc_backend_mlvc.so" -exec cp {} "$DEPLOY/lib/rkvc/backends/" \;
fi

aarch64-linux-gnu-strip "$DEPLOY/bin/rkvc" "$DEPLOY/lib/librkvc.so.0.4.0" \
    "$DEPLOY/lib/rkvc/rkvc_backend_mlvc.so" 2>/dev/null || true
echo "DEPLOY-OK $DEPLOY"
find "$DEPLOY" -type f | sort
