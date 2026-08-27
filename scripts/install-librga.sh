#!/bin/bash
# scripts/install-librga.sh — 从 third_party/librga 子模块安装头文件、动态库与 pkg-config
#
# 默认安装到源码树 .build/deps/librga-install（无需 sudo，也不再依赖系统预装 librga）。
# ffmpeg --enable-rkrga 与 rkvc 链接均通过该前缀发现 librga。
#
# 用法:
#   ./scripts/install-librga.sh
#   PREFIX=/usr/local ./scripts/install-librga.sh   # 可选：装到系统前缀（需写权限）
#   RKVC_TARGET_ARCH=aarch64 ./scripts/install-librga.sh  # x86 交叉打包

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX="${PREFIX:-$PROJECT_DIR/.build/deps/librga-install}"
SRC_DIR="${SRC_DIR:-$PROJECT_DIR/third_party/librga}"

ARCH="${RKVC_TARGET_ARCH:-$(uname -m)}"
case "$ARCH" in
    aarch64|arm64) LIB_SUBDIR=gcc-aarch64 ;;
    armv7l|armhf)  LIB_SUBDIR=gcc-armhf ;;
    *)
        echo "错误: 不支持的架构: $ARCH（librga 预编译库仅提供 aarch64/armhf）"
        exit 1
        ;;
esac

if [[ ! -f "$SRC_DIR/include/im2d.h" || ! -f "$SRC_DIR/libs/Linux/$LIB_SUBDIR/librga.so" ]]; then
    echo "错误: librga 子模块未初始化或不完整: $SRC_DIR"
    echo "  运行: git submodule update --init --depth 1 third_party/librga"
    exit 1
fi

install -d "$PREFIX/include/rga"
install -m 644 "$SRC_DIR/include/"*.h "$PREFIX/include/rga/"

install -d "$PREFIX/lib"
install -m 755 "$SRC_DIR/libs/Linux/$LIB_SUBDIR/librga.so" "$PREFIX/lib/"
if [[ -f "$SRC_DIR/libs/Linux/$LIB_SUBDIR/librga.a" ]]; then
    install -m 644 "$SRC_DIR/libs/Linux/$LIB_SUBDIR/librga.a" "$PREFIX/lib/"
fi

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

# 仅当装到系统路径且 ldconfig 可用时刷新缓存
if [[ "$PREFIX" == /usr || "$PREFIX" == /usr/local ]] && command -v ldconfig &>/dev/null; then
    ldconfig
fi

echo "--- librga $VERSION 已安装到 $PREFIX (arch=$ARCH, from submodule) ---"
