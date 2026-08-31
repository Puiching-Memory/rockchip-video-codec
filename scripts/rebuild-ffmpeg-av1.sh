#!/bin/bash
# scripts/rebuild-ffmpeg-av1.sh — 构建 ffmpeg-rockchip (AV1 硬解 + libsvtav1 软编 + 封装)
#
# AV1 编码经 FFmpeg libsvtav1 链接 third_party/SVT-AV1。
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
SVT_PREFIX="${SVT_PREFIX:-$PROJECT_DIR/.build/deps/svt-av1-install}"
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

    export PKG_CONFIG_PATH="$SVT_PREFIX/lib/pkgconfig:$MPP_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$SVT_PREFIX/lib:$MPP_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    cd "$FFMPEG_SRC"

    if [[ $CLEAN -eq 1 ]]; then
        make distclean 2>/dev/null || true
        [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]] && rm -rf "$FFMPEG_PREFIX"
    fi

    # CI（Ubuntu 22.04）镜像自带 libbz2，autodetect 会启用 bzlib，
    # 静态 libavcodec 因此引用 BZ2_* 符号且不会随库传播，请显式关闭。
    ./configure \
        "${extra_configure[@]}" \
        --enable-version3 \
        --enable-rkmpp --enable-libdrm \
        --enable-libsvtav1 \
        --enable-pic \
        --disable-doc --disable-programs --disable-network \
        --disable-bzlib \
        --enable-swscale --disable-swresample \
        --disable-x86asm \
        --disable-everything \
        --enable-decoder=av1_rkmpp --enable-decoder=libaom-av1 \
        --enable-encoder=libsvtav1 \
        --enable-muxer=mp4 --enable-muxer=matroska --enable-muxer=mpegts --enable-muxer=ivf \
        --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=mpegts --enable-demuxer=ivf \
        --enable-parser=av1 \
        --enable-protocol=file --enable-protocol=pipe

    make -j"$BUILD_JOBS"
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        make install
    fi
    echo "--- ffmpeg 构建完成 (av1_rkmpp + libsvtav1) ---"
}

main() {
    echo "=== rebuild-ffmpeg-av1 (prefix=$FFMPEG_PREFIX) ==="
    if [[ ! -f "$MPP_PREFIX/lib/librockchip_mpp.so" ]]; then
        echo "错误: 请先构建 MPP (.build/deps/mpp-install 或 package-portable.sh)"
        exit 1
    fi
    if [[ ! -f "$SVT_PREFIX/lib/libSvtAv1Enc.so" ]] && \
       ! ls "$SVT_PREFIX"/lib/libSvtAv1Enc.so.* >/dev/null 2>&1; then
        echo "--- 构建 SVT-AV1 到 $SVT_PREFIX ---"
        "$SCRIPT_DIR/build-svt.sh"
    fi
    if ! PKG_CONFIG_PATH="$SVT_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
            pkg-config --exists SvtAv1Enc; then
        echo "错误: pkg-config 找不到 SvtAv1Enc（期望 $SVT_PREFIX/lib/pkgconfig/SvtAv1Enc.pc）"
        exit 1
    fi
    rkvc_apply_ffmpeg_patches "$FFMPEG_SRC"
    configure_ffmpeg
}

main
