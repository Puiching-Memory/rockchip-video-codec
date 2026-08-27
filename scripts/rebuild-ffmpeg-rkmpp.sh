#!/bin/bash
# scripts/rebuild-ffmpeg-rkmpp.sh — ffmpeg-rockchip 全量 RKMPP (H.264/HEVC/AV1)
#
# 解码: h264_rkmpp, hevc_rkmpp, av1_rkmpp + 软解 h264/hevc/rawvideo
# 编码: h264_rkmpp, hevc_rkmpp, libsvtav1 (SVT-AV1)
# 滤镜: scale, hwdownload, scale_rkrga, psnr, ssim
# 构建前自动应用 patches/ffmpeg-rockchip/*.patch（ROI / 运行时 RC 等）
# 修改 configure 选项后请使用 --clean 重编；升级 MPP/RGA/SVT 依赖后同样建议 --clean。
#
# 缓存：构建成功后在 $FFMPEG_PREFIX/.rkvc-ffmpeg.stamp 写入指纹（子模块 commit +
# 补丁哈希 + configure 选项 + 依赖前缀路径），下次运行指纹命中则直接跳过；
# 构建中断不会写 stamp，下次自动重建。
#
# 用法:
#   ./scripts/rebuild-ffmpeg-rkmpp.sh [--clean] [--prefix DIR]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FFMPEG_SRC="${FFMPEG_SRC:-$PROJECT_DIR/third_party/ffmpeg-rockchip}"
MPP_PREFIX="${MPP_PREFIX:-$PROJECT_DIR/.build/deps/mpp-install}"
RGA_PREFIX="${RGA_PREFIX:-$PROJECT_DIR/.build/deps/librga-install}"
SVT_PREFIX="${SVT_PREFIX:-$PROJECT_DIR/.build/deps/svt-av1-install}"
FFMPEG_PREFIX="${FFMPEG_PREFIX:-$FFMPEG_SRC}"
FFMPEG_BUILD_DIR="${FFMPEG_BUILD_DIR:-$FFMPEG_SRC}"

CLEAN=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --prefix) FFMPEG_PREFIX="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

STAMP_NAME=".rkvc-ffmpeg.stamp"

# 固定 configure 选项：同时参与指纹与构建，增删选项会自动触发重建。
FFMPEG_CONFIGURE_FLAGS=(
    --enable-version3
    --enable-rkmpp --enable-libdrm --enable-rkrga
    --enable-libsvtav1
    --enable-pic
    --disable-doc --enable-ffmpeg --enable-ffprobe --disable-network
    --enable-swscale --disable-swresample
    --disable-x86asm
    --disable-everything
    --enable-decoder=h264_rkmpp --enable-decoder=hevc_rkmpp
    --enable-decoder=av1_rkmpp
    --enable-decoder=h264 --enable-decoder=hevc
    --enable-decoder=rawvideo
    --enable-encoder=h264_rkmpp --enable-encoder=hevc_rkmpp
    --enable-encoder=libsvtav1
    --enable-encoder=rawvideo --enable-encoder=wrapped_avframe
    --enable-parser=h264 --enable-parser=hevc --enable-parser=av1
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb
    --enable-muxer=mp4 --enable-muxer=matroska --enable-muxer=mpegts --enable-muxer=ivf
    --enable-muxer=yuv4mpegpipe --enable-muxer=rawvideo --enable-muxer=null
    --enable-demuxer=mov --enable-demuxer=matroska --enable-demuxer=mpegts --enable-demuxer=ivf
    --enable-demuxer=yuv4mpegpipe --enable-demuxer=rawvideo --enable-demuxer=h264 --enable-demuxer=hevc
    --enable-protocol=file --enable-protocol=pipe
    --enable-filter=psnr --enable-filter=ssim --enable-filter=format --enable-filter=scale
    --enable-filter=hwdownload --enable-filter=hwupload --enable-filter=setsar
    --enable-filter=scale_rkrga
)

# 影响产物的全部输入拼接后取 SHA-256：子模块 commit、补丁内容、configure 选项、依赖前缀。
ffmpeg_fingerprint() {
    local commit source_diff p
    commit="$(git -C "$FFMPEG_SRC" rev-parse HEAD 2>/dev/null || echo unknown)"
    source_diff="$(git -C "$FFMPEG_SRC" diff --binary -- . 2>/dev/null | sha256sum | awk '{print $1}')"
    {
        printf 'commit=%s\n' "$commit"
        printf 'source_diff=%s\n' "$source_diff"
        printf 'prefix=%s\nbuild=%s\nmpp=%s\nrga=%s\nsvt=%s\n' \
            "$FFMPEG_PREFIX" "$FFMPEG_BUILD_DIR" "$MPP_PREFIX" "$RGA_PREFIX" "$SVT_PREFIX"
        printf 'target=%s\ncross_prefix=%s\nsysroot=%s\n' \
            "${RKVC_TARGET_ARCH:-native}" "${RKVC_CROSS_PREFIX:-}" "${RKVC_SYSROOT:-}"
        printf 'flags=%s\n' "${FFMPEG_CONFIGURE_FLAGS[*]}"
        for p in "$PROJECT_DIR"/patches/ffmpeg-rockchip/*.patch; do
            [[ -f "$p" ]] || continue
            printf 'patch=%s %s\n' "$(basename "$p")" "$(sha256sum "$p" | awk '{print $1}')"
        done
    } | sha256sum | awk '{print $1}'
}

configure_ffmpeg() {
    local extra_configure=(--enable-static --enable-shared)
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        extra_configure+=(--prefix="$FFMPEG_PREFIX")
    fi

    local cross_configure=()
    if rkvc_cross_enabled; then
        rkvc_require_cross_tools
        local cross_prefix="${RKVC_CROSS_PREFIX:-$(rkvc_target_triplet)-}"
        cross_configure+=(
            --enable-cross-compile
            --target-os=linux
            --arch=aarch64
            --cross-prefix="$cross_prefix"
            --pkg-config=pkg-config
        )
        [[ -n "${RKVC_SYSROOT:-}" ]] && cross_configure+=(--sysroot="$RKVC_SYSROOT")
        export PKG_CONFIG_LIBDIR="$(rkvc_target_pkg_config_libdir)"
    fi

    export PKG_CONFIG_PATH="$SVT_PREFIX/lib/pkgconfig:$RGA_PREFIX/lib/pkgconfig:$MPP_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    export LD_LIBRARY_PATH="$SVT_PREFIX/lib:$RGA_PREFIX/lib:$MPP_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

    # FFmpeg refuses an out-of-tree configure while config.h/config.mak from a
    # previous in-source build exists.  Cross outputs live in their own build
    # tree, so clear only generated source-tree build files first.
    if [[ "$FFMPEG_BUILD_DIR" != "$FFMPEG_SRC" && \
          ( -f "$FFMPEG_SRC/config.h" || -f "$FFMPEG_SRC/ffbuild/config.mak" ) ]]; then
        echo "--- 清理 ffmpeg 源码树中的旧原生构建产物（交叉构建使用隔离目录）---"
        (cd "$FFMPEG_SRC" && make distclean >/dev/null 2>&1) || {
            echo "错误: 无法清理 ffmpeg 源码树旧配置，请手动运行 make distclean" >&2
            return 1
        }
    fi

    mkdir -p "$FFMPEG_BUILD_DIR"
    cd "$FFMPEG_BUILD_DIR"

    if [[ $CLEAN -eq 1 ]]; then
        make distclean 2>/dev/null || true
        [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]] && rm -rf "$FFMPEG_PREFIX"
        mkdir -p "$FFMPEG_BUILD_DIR"
        cd "$FFMPEG_BUILD_DIR"
    fi

    "$FFMPEG_SRC/configure" \
        "${extra_configure[@]}" \
        "${cross_configure[@]+"${cross_configure[@]}"}" \
        "${FFMPEG_CONFIGURE_FLAGS[@]}"

    make -j"$BUILD_JOBS"
    if [[ "$FFMPEG_PREFIX" != "$FFMPEG_SRC" ]]; then
        make install
    fi
    echo "--- ffmpeg 构建完成 (h264/hevc/av1 rkmpp + libsvtav1) ---"
}

ffmpeg_patches_applied() {
    local patch
    for patch in "$PROJECT_DIR"/patches/ffmpeg-rockchip/*.patch; do
        [[ -f "$patch" ]] || continue
        # Both canonical source states are valid for an installed-prefix cache:
        # clean (patch can be applied) and temporarily patched (reverse works).
        # Any conflicting local edit invalidates the cache; source_diff is also
        # part of the fingerprint above.
        if git -C "$FFMPEG_SRC" apply --reverse --check "$patch" >/dev/null 2>&1; then
            continue
        fi
        git -C "$FFMPEG_SRC" apply --check "$patch" >/dev/null 2>&1 || return 1
    done
    return 0
}

main() {
    echo "=== rebuild-ffmpeg-rkmpp (prefix=$FFMPEG_PREFIX) ==="
    local stamp="$FFMPEG_PREFIX/$STAMP_NAME"
    if [[ $CLEAN -eq 1 ]]; then
        rm -f "$stamp"
    fi
    local fingerprint
    fingerprint="$(ffmpeg_fingerprint)"
    # 指纹命中且产物存在 → 直接跳过（不打补丁、不碰源码树）；
    # 构建中断不会写 stamp，故半成品必然重建。
    if [[ $CLEAN -eq 0 && -f "$stamp" && "$(cat "$stamp")" == "$fingerprint" ]] \
       && ls "$FFMPEG_PREFIX/lib"/libavcodec.so.* >/dev/null 2>&1 \
       && ffmpeg_patches_applied; then
        echo "--- ffmpeg 已构建，跳过: $FFMPEG_PREFIX (用 --clean 重建) ---"
        exit 0
    fi
    if [[ $CLEAN -eq 0 && -f "$stamp" && "$(cat "$stamp")" == "$fingerprint" ]] \
       && ! ffmpeg_patches_applied; then
        echo "--- ffmpeg 缓存失效: 源码树与已登记补丁冲突 ---"
    fi
    if [[ $CLEAN -eq 0 ]]; then
        echo "--- ffmpeg 缓存未命中: stamp=$(cat "$stamp" 2>/dev/null || echo '<无>') fp=$fingerprint ---"
    fi
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
    printf '%s\n' "$fingerprint" > "$stamp"
    echo "--- 缓存指纹已写入: $stamp ---"
}

main
