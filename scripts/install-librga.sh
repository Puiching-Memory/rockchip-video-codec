#!/bin/bash
# scripts/install-librga.sh — 安装 librga 头文件、动态库和 pkg-config 文件
#
# Ubuntu 24.04 无 librga-dev 包；ffmpeg --enable-rkrga 需要 pkg-config 可发现 librga。
# 用法: sudo ./scripts/install-librga.sh

set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
ARCH="$(uname -m)"
case "$ARCH" in
    aarch64|arm64) LIB_SUBDIR=gcc-aarch64 ;;
    x86_64|amd64)  LIB_SUBDIR=gcc-x86_64 ;;
    *) echo "错误: 不支持的架构: $ARCH"; exit 1 ;;
esac

SRC_DIR="${TMPDIR:-/tmp}/librga"
if [[ ! -d "$SRC_DIR/.git" ]]; then
    git clone --depth 1 https://github.com/airockchip/librga.git "$SRC_DIR"
fi

install -d "$PREFIX/include/rga"
install -m 644 "$SRC_DIR/include/"*.h "$PREFIX/include/rga/"

install -d "$PREFIX/lib"
install -m 755 "$SRC_DIR/libs/Linux/$LIB_SUBDIR/librga.so" "$PREFIX/lib/"

VERSION="$(grep '#define RGA_API_MAJOR_VERSION' "$SRC_DIR/include/im2d_version.h" | awk '{print $3}').$(grep '#define RGA_API_MINOR_VERSION' "$SRC_DIR/include/im2d_version.h" | awk '{print $3}').$(grep '#define RGA_API_REVISION_VERSION' "$SRC_DIR/include/im2d_version.h" | awk '{print $3}')"

install -d "$PREFIX/lib/pkgconfig"
cat > "$PREFIX/lib/pkgconfig/librga.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: librga
Description: Rockchip 2D Raster Graphic Acceleration
Version: $VERSION
Libs: -L\${libdir} -lrga
Cflags: -I\${includedir}
EOF

ldconfig
echo "--- librga $VERSION 已安装到 $PREFIX (arch=$ARCH) ---"
