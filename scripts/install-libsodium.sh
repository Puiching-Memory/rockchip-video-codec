#!/bin/bash
# scripts/install-libsodium.sh — 从 third_party/libsodium 子模块源码构建并安装静态库
#
# libsodium 使用 autotools 构建系统（无 CMakeLists），沿用本项目对非 CMake
# 子模块（librga / SVT-AV1）的「install 脚本 + 前缀」惯例。
# 默认安装到源码树 .build/deps/libsodium-install（无需 sudo），
# 提供 Ed25519 签名/验签与 SHA-256（1机1码授权所需），供 librkvc 与 rkvc_lic 静态链接。
#
# 用法:
#   ./scripts/install-libsodium.sh
#   PREFIX=/usr/local ./scripts/install-libsodium.sh   # 装到系统前缀（需写权限）
#   BUILD_JOBS=8 ./scripts/install-libsodium.sh         # 指定并行编译任务数
#   RKVC_TARGET_ARCH=aarch64 BUILD_DIR=... PREFIX=... ./scripts/install-libsodium.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX="${PREFIX:-$PROJECT_DIR/.build/deps/libsodium-install}"
SRC_DIR="${SRC_DIR:-$PROJECT_DIR/third_party/libsodium}"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/.build/deps/libsodium-build}"
NPROCS="${NPROCS:-$BUILD_JOBS}"
AUTOTOOLS_HOST="${RKVC_AUTOTOOLS_HOST:-}"
if [[ -z "$AUTOTOOLS_HOST" ]] && rkvc_cross_enabled; then
    AUTOTOOLS_HOST="$(rkvc_target_triplet)"
fi

if [[ ! -f "$SRC_DIR/configure.ac" ]]; then
    echo "错误: libsodium 子模块未初始化或不完整: $SRC_DIR"
    echo "  运行: git submodule update --init third_party/libsodium"
    exit 1
fi

# autotools 工具链检查
for tool in autoreconf automake autoconf libtoolize; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "错误: 缺少 autotools 工具 '$tool'（需要 autoconf/automake/libtool）"
        exit 1
    fi
done

echo "==> 构建 libsodium（源码: $SRC_DIR）"
echo "==> 构建目录: $BUILD_DIR"
echo "==> 安装前缀: $PREFIX"
[[ -n "$AUTOTOOLS_HOST" ]] && echo "==> 目标 triplet: $AUTOTOOLS_HOST"

# 生成 configure（仅首次或 configure.ac 变更时需要）
if [[ ! -f "$SRC_DIR/configure" ]]; then
    echo "==> autoreconf -i"
    (cd "$SRC_DIR" && autoreconf -i)
fi

# 在源码树外构建，避免 Makefile/.libs/*.o 污染子模块，也允许不同前缀重配。
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
echo "==> configure --prefix=$PREFIX --disable-shared --enable-static"
configure_args=()
[[ -n "$AUTOTOOLS_HOST" ]] && configure_args+=(--host="$AUTOTOOLS_HOST")
"$SRC_DIR/configure" \
    --prefix="$PREFIX" \
    --disable-shared \
    --enable-static \
    --with-pic \
    --disable-dependency-tracking \
    "${configure_args[@]+"${configure_args[@]}"}"

echo "==> make -j${NPROCS}"
make -j"$NPROCS"

echo "==> make install"
make install

echo
echo "✓ libsodium 已安装到 $PREFIX"
echo "  静态库: $PREFIX/lib/libsodium.a"
echo "  头文件: $PREFIX/include/sodium.h"
