#!/bin/bash
# scripts/package-portable.sh — 从源码构建可移植二进制包
#
# 用法:
#   ./scripts/package-portable.sh [--clean]
#
# 流程:
#   1. 从 third_party/mpp 子模块编译 rockchip-mpp
#   2. 从 third_party/librga 子模块安装预编译 librga
#   3. 从 third_party/ffmpeg-rockchip 子模块编译 ffmpeg
#   4. 用编译的 ffmpeg / MPP / librga 构建 rkvc
#   5. bundle 动态库（含 librga）+ RPATH 打包
#
# 前置依赖: gcc, g++, cmake, make, pkg-config, patchelf, libdrm-dev
# 可选依赖: ninja (若已有 ninja 构建目录则自动使用)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
FFMPEG_SRC="$PROJECT_DIR/third_party/ffmpeg-rockchip"
MPP_SRC="$PROJECT_DIR/third_party/mpp"
RGA_SRC="$PROJECT_DIR/third_party/librga"
SVT_PREFIX="$PROJECT_DIR/.build/deps/svt-av1-install"
FFMPEG_PREFIX="$PROJECT_DIR/.build/deps/ffmpeg-install"
MPP_BUILD="$PROJECT_DIR/.build/deps/mpp-build"
MPP_PREFIX="$PROJECT_DIR/.build/deps/mpp-install"
RGA_PREFIX="$PROJECT_DIR/.build/deps/librga-install"
RKVC_BUILD="$PROJECT_DIR/.build/portable"
OUT_DIR="$PROJECT_DIR/.build/dist"

VERSION="$(rkvc_project_version)"
ARCH="$(uname -m)"
PKG_NAME="$(rkvc_portable_pkg_dir)"

CLEAN=0
[[ "${1:-}" == "--clean" ]] && CLEAN=1

# 自动检测 CMake 生成器: 若系统有 ninja 且 build 目录用 Ninja 则使用 Ninja，否则 Unix Makefiles
detect_generator() {
    local build_dir="${1:-$RKVC_BUILD}"
    local cache="$build_dir/CMakeCache.txt"
    if [[ -f "$cache" ]] && grep -q "CMAKE_MAKE_PROGRAM.*ninja" "$cache"; then
        echo "Ninja"
    elif command -v ninja &>/dev/null; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

detect_build_cmd() {
    local build_dir="${1:-$RKVC_BUILD}"
    local cache="$build_dir/CMakeCache.txt"
    if [[ -f "$cache" ]] && grep -q "CMAKE_MAKE_PROGRAM.*ninja" "$cache"; then
        echo "ninja"
    else
        echo "make"
    fi
}

check_deps() {
    local missing=()
    for cmd in gcc g++ cmake make pkg-config patchelf; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "错误: 缺少工具: ${missing[*]}"
        exit 1
    fi
    if [[ ! -f "$FFMPEG_SRC/configure" ]]; then
        echo "错误: ffmpeg 子模块未初始化，运行: git submodule update --init --depth 1 third_party/ffmpeg-rockchip"
        exit 1
    fi
    if [[ ! -f "$MPP_SRC/CMakeLists.txt" ]]; then
        echo "错误: mpp 子模块未初始化，运行: git submodule update --init --depth 1 third_party/mpp"
        exit 1
    fi
    if [[ ! -f "$RGA_SRC/include/im2d.h" ]]; then
        echo "错误: librga 子模块未初始化，运行: git submodule update --init --depth 1 third_party/librga"
        exit 1
    fi
}

build_mpp() {
    echo "=== 构建 rockchip-mpp ==="

    if [[ $CLEAN -eq 1 ]]; then
        rm -rf "$MPP_BUILD" "$MPP_PREFIX"
    fi

    if [[ -f "$MPP_BUILD/mpp/librockchip_mpp.so" && -f "$MPP_PREFIX/lib/librockchip_mpp.so" ]]; then
        echo "--- rockchip-mpp 已构建，跳过 (用 --clean 重建) ---"
        return
    fi

    local gen
    gen="$(detect_generator "$MPP_BUILD")"
    echo "--- 使用生成器: $gen ---"

    cmake -S "$MPP_SRC" -B "$MPP_BUILD" -G "$gen" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
        -DBUILD_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$MPP_PREFIX"

    local build_cmd
    build_cmd="$(detect_build_cmd "$MPP_BUILD")"
    $build_cmd -C "$MPP_BUILD" -j"$BUILD_JOBS"
    $build_cmd -C "$MPP_BUILD" install

    echo "--- rockchip-mpp 构建完成 ---"
}

build_svt() {
    "$SCRIPT_DIR/build-svt.sh" ${CLEAN:+--clean}
}

install_rga() {
    echo "=== 安装 librga (submodule → $RGA_PREFIX) ==="
    if [[ $CLEAN -eq 1 ]]; then
        rm -rf "$RGA_PREFIX"
    fi
    if [[ -f "$RGA_PREFIX/lib/librga.so" ]]; then
        echo "--- librga 已安装，跳过 (用 --clean 重建) ---"
        return
    fi
    PREFIX="$RGA_PREFIX" "$SCRIPT_DIR/install-librga.sh"
}

build_ffmpeg() {
    echo "=== 构建 ffmpeg-rockchip (AV1 硬解) ==="
    RGA_PREFIX="$RGA_PREFIX" MPP_PREFIX="$MPP_PREFIX" \
        "$SCRIPT_DIR/rebuild-ffmpeg-rkmpp.sh" ${CLEAN:+--clean} --prefix "$FFMPEG_PREFIX"
}

build_rkvc() {
    echo ""
    echo "=== 构建 rkvc (.build/portable/) ==="
    # 目录与 CMakePresets.json「portable」一致；此处手写 -B，兼容 CMake < 3.21
    [[ $CLEAN -eq 1 ]] && rm -rf "$RKVC_BUILD"

    local gen
    gen="$(detect_generator)"
    echo "--- 使用生成器: $gen ---"

    cmake -B "$RKVC_BUILD" -G "$gen" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
        -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
        -DMPP_BUILD_DIR="$MPP_BUILD" \
        -DRGA_PREFIX="$RGA_PREFIX" \
        "$PROJECT_DIR"

    local build_cmd
    build_cmd="$(detect_build_cmd)"
    $build_cmd -C "$RKVC_BUILD" -j"$BUILD_JOBS"
    echo "--- rkvc 构建完成 ---"
}

package() {
    echo ""
    echo "=== 打包 $PKG_NAME ==="

    rm -rf "$OUT_DIR/$PKG_NAME"
    mkdir -p "$OUT_DIR/$PKG_NAME"/{bin,lib,include/rkvc,share/pkgconfig,examples/bin,examples/src,models}

    for tool in rkvc_encode rkvc_decode rkvc_info rkvc_bench rkvc_transcode \
                rkvc_session_upscale rkvc_yuv_upscale; do
        [[ -f "$RKVC_BUILD/$tool" ]] && cp "$RKVC_BUILD/$tool" "$OUT_DIR/$PKG_NAME/bin/"
    done

    echo "--- 复制示例程序二进制 ---"
    for exe in "$RKVC_BUILD"/example_*; do
        [[ -f "$exe" ]] || continue
        cp "$exe" "$OUT_DIR/$PKG_NAME/examples/bin/"
        echo "  $(basename "$exe")"
    done

    echo "--- 复制示例程序源码 ---"
    for src in "$PROJECT_DIR/examples/"*; do
        [[ -f "$src" ]] || continue
        cp "$src" "$OUT_DIR/$PKG_NAME/examples/src/"
        echo "  $(basename "$src")"
    done

    # librkvc 版本号随项目变化，用通配符匹配
    local rkvc_real
    rkvc_real="$(ls -1 "$RKVC_BUILD"/librkvc.so.*.*.* 2>/dev/null | sort -V | tail -1)"
    if [[ -f "$rkvc_real" ]]; then
        cp -a "$rkvc_real" "$OUT_DIR/$PKG_NAME/lib/"
    fi
    # 符号链接由下方统一循环创建

    echo "--- 复制 ffmpeg 动态库 (仅限 rkvc 依赖) ---"
    # rkvc 依赖 libavcodec / libavformat / libavutil / libswscale
    # (libswscale 用于解码器在硬件无法直接输出请求格式时的软件像素格式转换)
    for name in libavcodec libavformat libavutil libswscale; do
        # 取最大版本号的真实文件
        local lib
        lib="$(ls -1 "$FFMPEG_PREFIX/lib/${name}.so."* 2>/dev/null | grep -v '\.so$' | sort -V | tail -1)"
        [[ -f "$lib" ]] || continue
        [[ -L "$lib" ]] && continue
        cp "$lib" "$OUT_DIR/$PKG_NAME/lib/"
        echo "  $(basename "$lib")"
    done

    echo "--- 复制 SVT-AV1 动态库 ---"
    for lib in "$SVT_PREFIX/lib"/libSvtAv1Enc.so.*; do
        [[ -f "$lib" ]] || continue
        [[ -L "$lib" ]] && continue
        cp "$lib" "$OUT_DIR/$PKG_NAME/lib/"
        echo "  $(basename "$lib")"
    done

    echo "--- 复制 rockchip-mpp 动态库 ---"
    for name in librockchip_mpp librockchip_vpu; do
        local soname_link="$MPP_PREFIX/lib/${name}.so.1"
        local real
        if [[ -L "$soname_link" ]]; then
            real="$(readlink -f "$soname_link")"
        else
            real="$(ls -1 "$MPP_PREFIX/lib/${name}.so."* 2>/dev/null | grep -v '\.so$' | sort -V | tail -1)"
        fi

        [[ -f "$real" ]] || continue
        cp "$real" "$OUT_DIR/$PKG_NAME/lib/"
        ln -sf "$(basename "$real")" "$OUT_DIR/$PKG_NAME/lib/${name}.so.1"
        ln -sf "${name}.so.1" "$OUT_DIR/$PKG_NAME/lib/${name}.so"
        echo "  $(basename "$real")"
    done

    # librga SONAME 为 librga.so（无版本后缀）
    echo "--- 复制 librga ---"
    if [[ -f "$RGA_PREFIX/lib/librga.so" ]]; then
        cp "$RGA_PREFIX/lib/librga.so" "$OUT_DIR/$PKG_NAME/lib/librga.so"
        echo "  librga.so  (from $RGA_PREFIX)"
    else
        echo "  错误: 未找到 $RGA_PREFIX/lib/librga.so"
        return 1
    fi

    # librknnrt SONAME 为 librknnrt.so（无版本后缀）；仅在 librkvc 链接了 RKNN 时打包
    echo "--- 复制 RKNN runtime (librknnrt) ---"
    local rkvc_for_rknn
    rkvc_for_rknn="$(ls -1 "$OUT_DIR/$PKG_NAME/lib"/librkvc.so.*.*.* 2>/dev/null | sort -V | tail -1)"
    if [[ -f "$rkvc_for_rknn" ]] && readelf -d "$rkvc_for_rknn" 2>/dev/null | grep -q 'NEEDED.*\[librknnrt\.so\]'; then
        local rknnrt=""
        for cand in \
            /usr/lib/aarch64-linux-gnu/librknnrt.so \
            /lib/aarch64-linux-gnu/librknnrt.so \
            /usr/lib/librknnrt.so \
            /usr/local/lib/librknnrt.so; do
            if [[ -f "$cand" ]]; then
                rknnrt="$(readlink -f "$cand")"
                break
            fi
        done
        if [[ -z "$rknnrt" ]]; then
            rknnrt="$(ldd "$rkvc_for_rknn" 2>/dev/null | awk '/librknnrt\.so/ {print $3; exit}')"
            [[ -n "$rknnrt" && -f "$rknnrt" ]] || rknnrt=""
        fi
        if [[ -n "$rknnrt" && -f "$rknnrt" ]]; then
            # SONAME=librknnrt.so，直接以该名放入包内，供 RPATH/$ORIGIN 解析
            cp "$rknnrt" "$OUT_DIR/$PKG_NAME/lib/librknnrt.so"
            echo "  librknnrt.so  (from $rknnrt)"
        else
            echo "  错误: librkvc 依赖 librknnrt.so，但构建机未找到该库"
            return 1
        fi

        echo "--- 复制 RKNN 超分模型 (models/) ---"
        local sr_model="$PROJECT_DIR/models/rkvc_sr_x3.crypt.rknn"
        if [[ -f "$sr_model" ]]; then
            cp "$sr_model" "$OUT_DIR/$PKG_NAME/models/rkvc_sr_x3.crypt.rknn"
            echo "  models/rkvc_sr_x3.crypt.rknn  (from $sr_model)"
        else
            echo "  错误: 已打包 librknnrt，但缺少约定模型: $sr_model"
            return 1
        fi
    else
        echo "  跳过 (本构建未启用 RKNN / librkvc 未链接 librknnrt)"
        echo "--- 跳过 RKNN 超分模型 (未链接 librknnrt) ---"
    fi

    cd "$OUT_DIR/$PKG_NAME/lib"
    for real in lib*.so.*; do
        [[ -f "$real" ]] || continue
        [[ -L "$real" ]] && continue
        [[ "$real" == librockchip_mpp.so.* || "$real" == librockchip_vpu.so.* ]] && continue
        # 标准化两级链接 (符合 Linux ld.so 惯例):
        #   libfoo.so → libfoo.so.X           (dev 链接)
        #   libfoo.so.X → libfoo.so.X.Y.Z     (SONAME 链接, ld.so 运行时使用)
        soname="${real%.*.*}"                # libfoo.so.X
        dev="${real%%.so.*}.so"              # libfoo.so
        ln -sf "$real"   "$soname" 2>/dev/null || true
        ln -sf "$soname" "$dev"    2>/dev/null || true
    done
    cd "$PROJECT_DIR"

    echo "--- 设置 RPATH ---"
    for tool in "$OUT_DIR/$PKG_NAME/bin/"*; do
        patchelf --set-rpath '$ORIGIN/../lib' "$tool" && \
            echo "  $(basename "$tool")"
    done
    for exe in "$OUT_DIR/$PKG_NAME/examples/bin/"*; do
        [[ -f "$exe" ]] || continue
        patchelf --set-rpath '$ORIGIN/../../lib' "$exe" && \
            echo "  examples/bin/$(basename "$exe")"
    done
    # librkvc 依赖 ffmpeg 库，也需要 $ORIGIN RPATH
    local rkvc_so
    rkvc_so="$(ls -1 "$OUT_DIR/$PKG_NAME/lib"/librkvc.so.*.*.* 2>/dev/null | sort -V | tail -1)"
    if [[ -f "$rkvc_so" ]]; then
        patchelf --set-rpath '$ORIGIN' "$rkvc_so" && \
            echo "  $(basename "$rkvc_so")"
    fi
    # ffmpeg 自身库之间也有依赖 (avcodec → avutil, swscale → avutil)
    for lib in "$OUT_DIR/$PKG_NAME/lib/"libav*.so.* "$OUT_DIR/$PKG_NAME/lib/"libswscale.so.*; do
        [[ -f "$lib" ]] || continue
        [[ -L "$lib" ]] && continue
        patchelf --set-rpath '$ORIGIN' "$lib" && \
            echo "  $(basename "$lib")"
    done
    for lib in "$OUT_DIR/$PKG_NAME/lib/"libSvtAv1Enc.so.*; do
        [[ -f "$lib" ]] || continue
        [[ -L "$lib" ]] && continue
        patchelf --set-rpath '$ORIGIN' "$lib" && \
            echo "  $(basename "$lib")"
    done
    for lib in "$OUT_DIR/$PKG_NAME/lib/"librockchip*.so.*; do
        [[ -f "$lib" ]] || continue
        [[ -L "$lib" ]] && continue
        patchelf --set-rpath '$ORIGIN' "$lib" && \
            echo "  $(basename "$lib")"
    done
    if [[ -f "$OUT_DIR/$PKG_NAME/lib/librknnrt.so" ]]; then
        patchelf --set-rpath '$ORIGIN' "$OUT_DIR/$PKG_NAME/lib/librknnrt.so" 2>/dev/null && \
            echo "  librknnrt.so" || true
    fi
    if [[ -f "$OUT_DIR/$PKG_NAME/lib/librga.so" ]]; then
        patchelf --set-rpath '$ORIGIN' "$OUT_DIR/$PKG_NAME/lib/librga.so" 2>/dev/null && \
            echo "  librga.so" || true
    fi

    cp "$PROJECT_DIR"/include/rkvc/*.h "$OUT_DIR/$PKG_NAME/include/rkvc/"
    cat > "$OUT_DIR/$PKG_NAME/share/pkgconfig/rkvc.pc" <<EOF
prefix=\${pcfiledir}/../..
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: rkvc
Description: RK3588 AV1 Video Codec Library (SVT-AV1 + av1_rkmpp)
Version: $VERSION
Libs: -L\${libdir} -lrkvc
Cflags: -I\${includedir}
EOF

    echo "--- 复制发布文档 ---"
    if [[ -d "$PROJECT_DIR/docs/release" ]]; then
        cp -r "$PROJECT_DIR/docs/release/"* "$OUT_DIR/$PKG_NAME/" 2>/dev/null || true
        ls "$OUT_DIR/$PKG_NAME"/*.md 2>/dev/null | while read -r f; do
            echo "  $(basename "$f")"
        done
    fi

    echo "--- 复制一键测试脚本 ---"
    cp "$PROJECT_DIR/scripts/test-portable.sh" "$OUT_DIR/$PKG_NAME/test.sh"
    chmod +x "$OUT_DIR/$PKG_NAME/test.sh"
    echo "  test.sh"
    cp "$PROJECT_DIR/scripts/portable-test-helpers.sh" "$OUT_DIR/$PKG_NAME/portable-test-helpers.sh"
    echo "  portable-test-helpers.sh"
    cp "$PROJECT_DIR/scripts/network-e2e-test.sh" "$OUT_DIR/$PKG_NAME/network-e2e-test.sh"
    chmod +x "$OUT_DIR/$PKG_NAME/network-e2e-test.sh"
    echo "  network-e2e-test.sh"

    cd "$OUT_DIR"
    tar czf "$PKG_NAME.tar.gz" "$PKG_NAME"
    echo "  产物: $OUT_DIR/$PKG_NAME.tar.gz ($(du -h "$PKG_NAME.tar.gz" | cut -f1))"

    echo "--- 验证自包含库 ---"
    local unresolved=0
    local wrong_origin=0
    local ldd_output
    ldd_output="$(LD_LIBRARY_PATH="$OUT_DIR/$PKG_NAME/lib" \
        ldd "$OUT_DIR/$PKG_NAME/bin/rkvc_info" 2>&1)"

    while read -r line; do
        if echo "$line" | grep -q "not found"; then
            echo "  错误: $line"
            unresolved=1
        fi
    done <<< "$ldd_output"

    for lib in librkvc libavcodec libavformat libavutil libswscale libSvtAv1Enc librockchip_mpp librga librknnrt; do
        if echo "$ldd_output" | grep -q "$lib"; then
            if echo "$ldd_output" | grep "$lib" | grep -vq "$OUT_DIR/$PKG_NAME/lib/"; then
                echo "  错误: $lib 未解析到包内 lib/"
                echo "$ldd_output" | grep "$lib" | sed 's/^/    /'
                wrong_origin=1
            fi
        fi
    done

    if [[ $unresolved -eq 0 && $wrong_origin -eq 0 ]]; then
        echo "  OK: 所有依赖已解析"
    else
        echo "  错误: 存在未解析依赖或系统库串入"
        return 1
    fi

    echo "--- 目标板前置依赖 (须由系统包管理器提供) ---"
    echo "  libdrm2           (DRM 渲染)"
    echo "  NPU 驱动/固件     (rkvc_sr AI 超分; librknnrt + models/ 已随包携带)"
    echo "  /dev/rga          (RGA 设备节点; librga 已随包携带)"
    echo ""
    echo "  安装示例: sudo apt install libdrm-dev"
}

main() {
    echo "=== 可移植包构建 (rkvc $VERSION, $ARCH) ==="
    check_deps
    build_mpp
    build_svt
    install_rga
    build_ffmpeg
    build_rkvc
    package
    echo ""
    echo "=== 完成: $OUT_DIR/$PKG_NAME.tar.gz ==="
}

main
