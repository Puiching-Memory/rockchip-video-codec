#!/bin/bash
# scripts/build-common.sh — 构建脚本共用辅助函数
#
# 用法（在其它构建脚本中）:
#   source "$(dirname "$0")/build-common.sh"
#   rkvc_limit_build_jobs
#   ver="$(rkvc_project_version)"
#
# 默认并行度 = round(nproc × 80%)（至少 1，且不超过 nproc）。
# BUILD_JOBS 已设置则尊重；RKVC_BUILD_JOBS_MAX 若设置则封顶；RKVC_BUILD_JOBS_PCT 可改百分比。
#
# 构建目录约定（唯一权威：CMakePresets.json / docs/build-layout.md）:
#   .build/release/     default Release
#   .build/debug/       debug / tidy
#   .build/tests/       tests
#   .build/asan/        asan
#   .build/coverage/    coverage
#   .build/portable/    portable 编译树
#   .build/dist/        可移植包成品
#   .build/deps/        第三方依赖（MPP / SVT / FFmpeg）

# 仓库根目录（本文件位于 scripts/）
rkvc_repo_root() {
    local here
    here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
    cd "$here/.." && pwd
}

# 从 CMakeLists.txt project(rkvc VERSION x.y.z) 解析版本（唯一来源）
rkvc_project_version() {
    local root cmake ver
    root="$(rkvc_repo_root)"
    cmake="$root/CMakeLists.txt"
    ver="$(grep -A1 'project(rkvc' "$cmake" | grep 'VERSION' | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1)"
    if [[ -z "$ver" ]]; then
        echo "错误: 无法从 $cmake 解析 project(VERSION)" >&2
        return 1
    fi
    printf '%s\n' "$ver"
}

# 可移植包目录名: rkvc-<ver>-linux-<arch>-portable
rkvc_portable_pkg_dir() {
    # 可选平台参数：传平台名（如 rk3588）时输出按平台的包目录名，
    # 不传保留旧行为（按 uname -m 架构）。
    local ver arch
    ver="$(rkvc_project_version)" || return 1
    if [[ -n "${1:-}" ]]; then
        arch="$1"
    else
        arch="$(uname -m)"
    fi
    printf 'rkvc-%s-linux-%s-portable\n' "$ver" "$arch"
}

# Normalize the target CPU name used by cross-build scripts.  Empty/native
# keeps the historical host build; arm64 is accepted as an alias for aarch64.
rkvc_normalize_target_arch() {
    case "${1:-native}" in
        native|"") printf 'native\n' ;;
        aarch64|arm64) printf 'aarch64\n' ;;
        armhf|armv7l) printf 'armhf\n' ;;
        *)
            echo "错误: 不支持的目标架构 '$1'（可选: native / aarch64 / armhf）" >&2
            return 1
            ;;
    esac
}

rkvc_cross_enabled() {
    [[ -n "${RKVC_TARGET_ARCH:-}" && "${RKVC_TARGET_ARCH}" != native && \
       "${RKVC_TARGET_ARCH}" != "$(uname -m)" ]]
}

rkvc_target_triplet() {
    case "${RKVC_TARGET_ARCH:-native}" in
        aarch64|arm64) printf 'aarch64-linux-gnu\n' ;;
        armhf|armv7l) printf 'arm-linux-gnueabihf\n' ;;
        native) printf '%s\n' "$(uname -m)-linux-gnu" ;;
        *) return 1 ;;
    esac
}

# Colon-separated pkg-config roots for the target sysroot. Project-local
# dependency prefixes should be prepended by each caller.
rkvc_target_pkg_config_libdir() {
    local triplet sysroot
    triplet="$(rkvc_target_triplet)" || return 1
    sysroot="${RKVC_SYSROOT:-}"
    if [[ -n "$sysroot" ]]; then
        printf '%s\n' \
            "$sysroot/usr/lib/$triplet/pkgconfig:$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
    else
        printf '%s\n' \
            "/usr/lib/$triplet/pkgconfig:/usr/$triplet/lib/pkgconfig:/usr/share/pkgconfig"
    fi
}

rkvc_require_cross_tools() {
    rkvc_cross_enabled || return 0
    local prefix="${RKVC_CROSS_PREFIX:-$(rkvc_target_triplet)-}" tool missing=()
    for tool in gcc g++ ar ranlib strip; do
        command -v "${prefix}${tool}" >/dev/null 2>&1 || missing+=("${prefix}${tool}")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "错误: 缺少交叉工具: ${missing[*]}" >&2
        return 1
    fi
}

rkvc_nproc() {
    local n
    n="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 1)"
    if ! [[ "$n" =~ ^[0-9]+$ ]] || [ "$n" -lt 1 ]; then
        n=1
    fi
    printf '%s\n' "$n"
}

# round(nproc × pct / 100)，默认 80%；结果落在 [1, nproc]。
rkvc_default_build_jobs() {
    local ncpus pct jobs
    ncpus="$(rkvc_nproc)"
    pct="${RKVC_BUILD_JOBS_PCT:-80}"
    if ! [[ "$pct" =~ ^[0-9]+$ ]] || [ "$pct" -lt 1 ]; then
        pct=80
    fi
    if [ "$pct" -gt 100 ]; then
        pct=100
    fi
    jobs=$(( (ncpus * pct + 50) / 100 ))
    if [ "$jobs" -lt 1 ]; then
        jobs=1
    fi
    if [ "$jobs" -gt "$ncpus" ]; then
        jobs="$ncpus"
    fi
    printf '%s\n' "$jobs"
}

rkvc_limit_build_jobs() {
    local ncpus jobs explicit=0
    ncpus="$(rkvc_nproc)"

    if [[ "${BUILD_JOBS:-}" =~ ^[1-9][0-9]*$ ]]; then
        jobs="$BUILD_JOBS"
        explicit=1
    else
        jobs="$(rkvc_default_build_jobs)"
    fi

    if [[ "${RKVC_BUILD_JOBS_MAX:-}" =~ ^[1-9][0-9]*$ ]] && [ "$jobs" -gt "$RKVC_BUILD_JOBS_MAX" ]; then
        jobs="$RKVC_BUILD_JOBS_MAX"
    fi

    BUILD_JOBS="$jobs"
    export BUILD_JOBS
    export CMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS"

    if [ "$explicit" -eq 1 ]; then
        echo "--- BUILD_JOBS=$BUILD_JOBS (explicit, nproc=$ncpus) ---"
    else
        echo "--- BUILD_JOBS=$BUILD_JOBS (nproc=$ncpus × ${RKVC_BUILD_JOBS_PCT:-80}%) ---"
    fi
}

# 可移植包内 ffmpeg 动态库清单（rkvc 实际依赖；libswscale 用于软像素格式转换）。
# 打包复制与包测试共用，新增依赖只改这里。
RKVC_BUNDLED_FFMPEG_LIBS=(libavcodec libavformat libavutil libswscale)
# 可移植包必须自包含解析的全部库（ldd 来源校验用）。
RKVC_BUNDLED_ALL_LIBS=(librkvc "${RKVC_BUNDLED_FFMPEG_LIBS[@]}" libSvtAv1Enc librockchip_mpp librga librknnrt)

# 探测 SoC 名：DT compatible 最后一个 rockchip,<soc> 条目，截断 '-'。
# 与 lib/platform.c probe_soc_name 同规则；探测失败输出空。
rkvc_soc_name() {
    local entry soc=""
    while IFS= read -r entry; do
        [[ "$entry" == rockchip,* ]] && soc="${entry#rockchip,}"
    done < <(tr '\0' '\n' < /proc/device-tree/compatible 2>/dev/null)
    printf '%s\n' "${soc%%-*}"
}

# 测试二进制依赖的动态库搜索路径（与 CMake RKVC_DEP_LIB_DIRS 一致）。
#
# test_* 二进制把依赖库写进 DT_RUNPATH，但 DT_RUNPATH 不用于解析传递依赖
#（例如 libavcodec.so → libSvtAv1Enc.so.4），故不经 ctest 而直接运行 test_*
# 时仍需补回依赖库路径。返回以 ':' 分隔、仅含已存在目录的路径。
rkvc_dep_library_path() {
    local root ffmpeg_src dir
    root="$(rkvc_repo_root)"
    ffmpeg_src="$root/third_party/ffmpeg-rockchip"

    local dirs=()
    for dir in \
        "$ffmpeg_src/libavcodec" "$ffmpeg_src/libavformat" \
        "$ffmpeg_src/libavutil" "$ffmpeg_src/libswscale" \
        "$root/.build/deps/mpp-install/lib" \
        "$root/.build/deps/mpp-build/mpp" \
        "$root/.build/deps/svt-av1-install/lib" \
        "$root/.build/deps/librga-install/lib" \
        "$root/.build/deps/rknn-install/lib"; do
        [[ -d "$dir" ]] && dirs+=("$dir")
    done

    local IFS=:
    echo "${dirs[*]}"
}

# 追踪本次脚本应用过的补丁，供退出时自动还原（跨函数共享）。
_RKVC_FFMPEG_PATCHES_APPLIED=()
_RKVC_FFMPEG_PATCH_SRC=""

# 将父仓库 patches/ffmpeg-rockchip/*.patch 幂等应用到 ffmpeg-rockchip 源码树。
# 补丁归属父仓库，子模块 gitlink 始终保持干净：成功处理后注册 EXIT 陷阱，
# 脚本退出（成功或失败）时由 rkvc_restore_ffmpeg_clean 自动还原源码树。
rkvc_apply_ffmpeg_patches() {
    local ffmpeg_src="${1:-}"
    local root patch_dir patch
    root="$(rkvc_repo_root)"
    if [[ -z "$ffmpeg_src" ]]; then
        ffmpeg_src="$root/third_party/ffmpeg-rockchip"
    fi
    patch_dir="$root/patches/ffmpeg-rockchip"

    if [[ ! -d "$ffmpeg_src" ]]; then
        echo "错误: ffmpeg 源码目录不存在: $ffmpeg_src" >&2
        return 1
    fi
    if [[ ! -d "$patch_dir" ]]; then
        return 0
    fi

    shopt -s nullglob
    local patches=( "$patch_dir"/*.patch )
    shopt -u nullglob
    if [[ ${#patches[@]} -eq 0 ]]; then
        return 0
    fi

    echo "--- 应用 ffmpeg-rockchip 补丁 ($patch_dir) ---"
    for patch in "${patches[@]}"; do
        local base
        base="$(basename "$patch")"
        if git -C "$ffmpeg_src" apply --check "$patch" >/dev/null 2>&1; then
            git -C "$ffmpeg_src" apply "$patch"
            _RKVC_FFMPEG_PATCHES_APPLIED+=( "$patch" )
            echo "  applied: $base"
        elif git -C "$ffmpeg_src" apply --reverse --check "$patch" >/dev/null 2>&1; then
            _RKVC_FFMPEG_PATCHES_APPLIED+=( "$patch" )
            echo "  already applied: $base"
        else
            echo "错误: 无法应用补丁 $base（与当前 ffmpeg-rockchip 源码不匹配）" >&2
            return 1
        fi
    done

    _RKVC_FFMPEG_PATCH_SRC="$ffmpeg_src"

    # 成功处理后注册退出陷阱：脚本退出时自动还原子模块干净状态
    if [[ ${#_RKVC_FFMPEG_PATCHES_APPLIED[@]} -gt 0 ]]; then
        trap rkvc_restore_ffmpeg_clean EXIT
    fi
}

# 还原 ffmpeg-rockchip 源码树到打补丁前的干净状态。
# 逆序逐个反向应用补丁；作为脚本 EXIT 陷阱自动触发，无需手动调用。
rkvc_restore_ffmpeg_clean() {
    local src="$_RKVC_FFMPEG_PATCH_SRC"
    [[ -z "$src" || ! -d "$src" ]] && return 0

    local i patch base
    # 逆序还原，保证多补丁时应用/还原顺序一致
    for (( i=${#_RKVC_FFMPEG_PATCHES_APPLIED[@]}-1; i>=0; i-- )); do
        patch="${_RKVC_FFMPEG_PATCHES_APPLIED[$i]}"
        base="$(basename "$patch")"
        if git -C "$src" apply --reverse --check "$patch" >/dev/null 2>&1; then
            git -C "$src" apply --reverse "$patch"
            echo "  restored: $base"
        fi
    done

    # 安全兜底：若仍有已跟踪文件残留修改，强制还原
    if ! git -C "$src" diff --quiet -- 2>/dev/null; then
        git -C "$src" checkout -- . >/dev/null 2>&1 || true
    fi

    _RKVC_FFMPEG_PATCHES_APPLIED=()
}
