#!/bin/bash
# scripts/install-rknnrt.sh — 下载 Rockchip RKNN runtime（librknnrt + 头文件）
#
# librknnrt 是 Rockchip 专有二进制，本仓不 vendoring。默认安装到
# .build/deps/rknn-install（无需 sudo、不依赖板卡系统预装），供 CMake 链接
# 与 scripts/package-portable.sh 打进可移植包。
#
# 默认 REF 与 tools/ 里 rknn-toolkit2 2.3.2 对齐。目标机仍需 NPU 驱动/固件。
#
# 用法:
#   ./scripts/install-rknnrt.sh
#   ./scripts/install-rknnrt.sh --force
#   PREFIX=/usr/local ./scripts/install-rknnrt.sh      # 可选：系统前缀（需写权限）
#   RKNNRT_REF=v2.3.2 ./scripts/install-rknnrt.sh      # 覆盖 git ref / tag
#   RKNNRT_ARCH=aarch64 ./scripts/install-rknnrt.sh    # 交叉编译宿主上下载目标库
#
# 再分发须遵守 Rockchip SDK 许可；见 docs/packaging.md。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PREFIX="${PREFIX:-$PROJECT_DIR/.build/deps/rknn-install}"
# 与仓库根目录 pyproject.toml 的 rknn-toolkit2 版本对齐
RKNNRT_REF="${RKNNRT_REF:-v2.3.2}"
RKNNRT_REPO="${RKNNRT_REPO:-https://github.com/airockchip/rknn-toolkit2}"
FORCE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --force|-f) FORCE=1; shift ;;
        --prefix) PREFIX="$2"; shift 2 ;;
        --ref) RKNNRT_REF="$2"; shift 2 ;;
        --arch) RKNNRT_ARCH="$2"; shift 2 ;;
        -h|--help)
            sed -n '2,20p' "$0"
            exit 0
            ;;
        *)
            echo "未知参数: $1" >&2
            exit 2
            ;;
    esac
done

HOST_ARCH="$(uname -m)"
ARCH="${RKNNRT_ARCH:-$HOST_ARCH}"
case "$ARCH" in
    aarch64|arm64) ARCH_DIR=aarch64 ;;
    armv7l|armhf)  ARCH_DIR=armhf ;;
    *)
        echo "错误: 无对应的 Linux librknnrt（当前 arch=$ARCH）" >&2
        echo "  RKNN runtime 仅提供 aarch64 / armhf。交叉编译请设 RKNNRT_ARCH=aarch64" >&2
        exit 1
        ;;
esac

STAMP="$PREFIX/rknnrt.stamp"
EXPECT_STAMP="${RKNNRT_REF} ${ARCH_DIR}"

if [[ $FORCE -eq 0 ]] \
    && [[ -f "$PREFIX/lib/librknnrt.so" ]] \
    && [[ -f "$PREFIX/include/rknn_api.h" ]] \
    && [[ -f "$STAMP" ]] \
    && [[ "$(cat "$STAMP")" == "$EXPECT_STAMP" ]]; then
    echo "--- librknnrt 已安装: $PREFIX (ref=$RKNNRT_REF arch=$ARCH_DIR，用 --force 重下) ---"
    exit 0
fi

RUNTIME_BASE="rknpu2/runtime/Linux/librknn_api"
# github.com/.../raw/<ref>/path 会 302 到 raw.githubusercontent.com
RAW_BASE="${RKNNRT_REPO}/raw/${RKNNRT_REF}/${RUNTIME_BASE}"

download() {
    local url="$1" dest="$2"
    local tmp="${dest}.part"
    mkdir -p "$(dirname "$dest")"
    echo "  GET $url"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 --retry-delay 2 -o "$tmp" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$tmp" "$url"
    else
        echo "错误: 需要 curl 或 wget" >&2
        return 1
    fi
    mv "$tmp" "$dest"
}

echo "=== 安装 librknnrt (ref=$RKNNRT_REF arch=$ARCH_DIR → $PREFIX) ==="

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/rkvc-rknnrt.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

download "${RAW_BASE}/${ARCH_DIR}/librknnrt.so" "$WORKDIR/librknnrt.so"
download "${RAW_BASE}/include/rknn_api.h" "$WORKDIR/rknn_api.h"
download "${RAW_BASE}/include/rknn_custom_op.h" "$WORKDIR/rknn_custom_op.h"
# 可选头，缺失不致命
if ! download "${RAW_BASE}/include/rknn_matmul_api.h" "$WORKDIR/rknn_matmul_api.h" 2>/dev/null; then
    rm -f "$WORKDIR/rknn_matmul_api.h"
    echo "  (无 rknn_matmul_api.h，跳过)"
fi

if [[ ! -s "$WORKDIR/librknnrt.so" ]]; then
    echo "错误: 下载的 librknnrt.so 为空" >&2
    exit 1
fi
# 拒绝 HTML 错误页（GitHub 偶尔返回登录页）
if grep -q '<html' "$WORKDIR/librknnrt.so" 2>/dev/null; then
    echo "错误: 下载结果不是共享库（可能是 HTML 错误页）" >&2
    exit 1
fi
if command -v file >/dev/null 2>&1; then
    file_desc="$(file -b "$WORKDIR/librknnrt.so")"
    case "$file_desc" in
        *ELF*"shared object"*) ;;
        *)
            echo "错误: librknnrt.so 不是 ELF 共享库: $file_desc" >&2
            exit 1
            ;;
    esac
fi

install -d "$PREFIX/lib" "$PREFIX/include" "$PREFIX/lib/pkgconfig"
install -m 755 "$WORKDIR/librknnrt.so" "$PREFIX/lib/librknnrt.so"
install -m 644 "$WORKDIR/rknn_api.h" "$PREFIX/include/rknn_api.h"
install -m 644 "$WORKDIR/rknn_custom_op.h" "$PREFIX/include/rknn_custom_op.h"
if [[ -f "$WORKDIR/rknn_matmul_api.h" ]]; then
    install -m 644 "$WORKDIR/rknn_matmul_api.h" "$PREFIX/include/rknn_matmul_api.h"
fi

cat > "$PREFIX/lib/pkgconfig/librknnrt.pc" <<EOF
prefix=$PREFIX
exec_prefix=\${prefix}
libdir=\${prefix}/lib
includedir=\${prefix}/include

Name: librknnrt
Description: Rockchip RKNN NPU runtime
Version: ${RKNNRT_REF#v}
Libs: -L\${libdir} -lrknnrt
Cflags: -I\${includedir}
EOF

printf '%s\n' "$EXPECT_STAMP" > "$STAMP"

if [[ "$PREFIX" == /usr || "$PREFIX" == /usr/local ]] && command -v ldconfig &>/dev/null; then
    ldconfig
fi

echo "--- librknnrt $RKNNRT_REF 已安装到 $PREFIX (arch=$ARCH_DIR, from rknn-toolkit2) ---"
echo "  库: $PREFIX/lib/librknnrt.so"
echo "  头: $PREFIX/include/rknn_api.h"
