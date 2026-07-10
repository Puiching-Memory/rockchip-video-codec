#!/bin/bash
# scripts/rebuild-ffmpeg-rkmpp.sh — ffmpeg-rockchip 全量 RKMPP (H.264/HEVC/AV1)
#
# 解码: h264_rkmpp, hevc_rkmpp, av1_rkmpp + 软解 h264/hevc/rawvideo
# 编码: h264_rkmpp, hevc_rkmpp
# 滤镜: scale, hwdownload, scale_rkrga, psnr, ssim
# 构建前自动应用 patches/ffmpeg-rockchip/*.patch（ROI / 运行时 RC 等）
# 修改 configure 选项后请使用 --clean 重编
#
# 用法:
#   ./scripts/rebuild-ffmpeg-rkmpp.sh [--clean] [--prefix DIR]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FFMPEG_SRC="$PROJECT_DIR/third_party/ffmpeg-rockchip"
MPP_PREFIX="${MPP_PREFIX:-$PROJECT_DIR/.build/deps/mpp-install}"
RGA_PREFIX="${RGA_PREFIX:-$PROJECT_DIR/.build/deps/librga-install}"
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
    local extra_configure=(--enable-static --enable-shared)
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        extra_configure+=(--prefix="$FFMPEG_PREFIX")
    fi

    export PKG_CONFIG_PATH="$RGA_PREFIX/lib/pkgconfig:$MPP_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$RGA_PREFIX/lib:$MPP_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    cd "$FFMPEG_SRC"

    if [[ $CLEAN -eq 1 ]]; then
        make distclean 2>/dev/null || true
        [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]] && rm -rf "$FFMPEG_PREFIX"
    fi

    ./configure \
        "${extra_configure[@]}" \
        --enable-gpl --enable-version3 --enable-nonfree \
        --enable-rkmpp --enable-libdrm --enable-rkrga \
        --enable-pic \
        --disable-doc --enable-ffmpeg --enable-ffprobe --disable-network \
        --enable-swscale --disable-swresample \
        --disable-x86asm \
        --disable-everything \
        --enable-decoder=h264_rkmpp --enable-decoder=hevc_rkmpp \
        --enable-decoder=av1_rkmpp \
        --enable-decoder=h264 --enable-decoder=hevc \
        --enable-decoder=rawvideo \
        --enable-encoder=h264_rkmpp --enable-encoder=hevc_rkmpp \
        --enable-encoder=rawvideo --enable-encoder=wrapped_avframe \
        --enable-parser=h264 --enable-parser=hevc --enable-parser=av1 \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
        --enable-muxer=mp4 --enable-muxer=matroska --enable-muxer=mpegts --enable-muxer=ivf \
        --enable-muxer=yuv4mpegpipe --enable-muxer=rawvideo --enable-muxer=null \
        --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=mpegts --enable-demuxer=ivf \
        --enable-demuxer=yuv4mpegpipe --enable-demuxer=rawvideo --enable-demuxer=h264 --enable-demuxer=hevc \
        --enable-protocol=file --enable-protocol=pipe \
        --enable-filter=psnr --enable-filter=ssim --enable-filter=format --enable-filter=scale \
        --enable-filter=hwdownload --enable-filter=hwupload --enable-filter=setsar \
        --enable-filter=scale_rkrga

    make -j"$BUILD_JOBS"
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        make install
    fi
    echo "--- ffmpeg 构建完成 (h264/hevc/av1 rkmpp) ---"
}

main() {
    echo "=== rebuild-ffmpeg-rkmpp (prefix=$FFMPEG_PREFIX) ==="
    if [[ ! -f "$MPP_PREFIX/lib/librockchip_mpp.so" ]]; then
        echo "错误: 请先构建 MPP (.build/deps/mpp-install 或 package-portable.sh)"
        exit 1
    fi
    if [[ ! -f "$RGA_PREFIX/lib/librga.so" ]]; then
        echo "--- 安装 librga 子模块到 $RGA_PREFIX ---"
        PREFIX="$RGA_PREFIX" "$SCRIPT_DIR/install-librga.sh"
    fi
    if ! PKG_CONFIG_PATH="$RGA_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
            pkg-config --exists librga; then
        echo "错误: pkg-config 找不到 librga（期望 $RGA_PREFIX/lib/pkgconfig/librga.pc）"
        exit 1
    fi
    rkvc_apply_ffmpeg_patches "$FFMPEG_SRC"
    configure_ffmpeg
}

main
