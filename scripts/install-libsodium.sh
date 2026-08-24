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

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX="${PREFIX:-$PROJECT_DIR/.build/deps/libsodium-install}"
SRC_DIR="${SRC_DIR:-$PROJECT_DIR/third_party/libsodium}"
NPROCS="${NPROCS:-$BUILD_JOBS}"

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
echo "==> 安装前缀: $PREFIX"

cd "$SRC_DIR"

# 生成 configure（仅首次或 configure.ac 变更时需要）
if [[ ! -f configure ]]; then
    echo "==> autoreconf -i"
    autoreconf -i
fi

# 静态库优先（授权模块静态链接，部署无需额外 .so）
echo "==> ./configure --prefix=$PREFIX --disable-shared --enable-static"
./configure \
    --prefix="$PREFIX" \
    --disable-shared \
    --enable-static \
    --with-pic \
    --disable-dependency-tracking

echo "==> make -j${NPROCS}"
make -j"$NPROCS"

echo "==> make install"
make install

echo
echo "✓ libsodium 已安装到 $PREFIX"
echo "  静态库: $PREFIX/lib/libsodium.a"
echo "  头文件: $PREFIX/include/sodium.h"
echo
echo "开启授权构建:"
echo "  cmake --preset default -DRKVC_ENABLE_LICENSE=ON"
