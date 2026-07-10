#!/bin/bash
# scripts/rebuild-ffmpeg-av1.sh — 构建 ffmpeg-rockchip (仅 AV1 硬解 + 封装)
#
# SVT-AV1 编码由 librkvc 直连 third_party/SVT-AV1，不经过 FFmpeg。
#
# 用法:
#   ./scripts/rebuild-ffmpeg-av1.sh [--clean] [--prefix DIR]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FFMPEG_SRC="$PROJECT_DIR/third_party/ffmpeg-rockchip"
MPP_PREFIX="${MPP_PREFIX:-$PROJECT_DIR/.build/deps/mpp-install}"
FFMPEG_PREFIX="$FFMPEG_SRC"

CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --prefix) FFMPEG_PREFIX="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

configure_ffmpeg() {
    local extra_configure=()
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        extra_configure+=(--prefix="$FFMPEG_PREFIX")
        extra_configure+=(--enable-static --enable-shared)
    fi

    export PKG_CONFIG_PATH="$MPP_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$MPP_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    cd "$FFMPEG_SRC"

    if [[ $CLEAN -eq 1 ]]; then
        make distclean 2>/dev/null || true
        [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]] && rm -rf "$FFMPEG_PREFIX"
    fi

    ./configure \
        "${extra_configure[@]}" \
        --enable-gpl --enable-version3 --enable-nonfree \
        --enable-rkmpp --enable-libdrm \
        --enable-pic \
        --disable-doc --disable-programs --disable-network \
        --enable-swscale --disable-swresample \
        --disable-x86asm \
        --disable-everything \
        --enable-decoder=av1_rkmpp --enable-decoder=libaom-av1 \
        --enable-muxer=mp4 --enable-muxer=matroska --enable-muxer=mpegts --enable-muxer=ivf \
        --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=mpegts --enable-demuxer=ivf \
        --enable-parser=av1 \
        --enable-protocol=file --enable-protocol=pipe

    make -j"$BUILD_JOBS"
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        make install
    fi
    echo "--- ffmpeg 构建完成 (av1_rkmpp decode only) ---"
}

main() {
    echo "=== rebuild-ffmpeg-av1 (prefix=$FFMPEG_PREFIX) ==="
    if [[ ! -f "$MPP_PREFIX/lib/librockchip_mpp.so" ]]; then
        echo "错误: 请先构建 MPP (.build/deps/mpp-install 或 package-portable.sh)"
        exit 1
    fi
    rkvc_apply_ffmpeg_patches "$FFMPEG_SRC"
    configure_ffmpeg
}

main
