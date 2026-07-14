#!/bin/bash
# scripts/build-common.sh — 构建脚本共用辅助函数
#
# 用法（在其它构建脚本中）:
#   source "$(dirname "$0")/build-common.sh"
#   rkvc_limit_build_jobs
#   ver="$(rkvc_project_version)"
#
# 可通过环境变量 BUILD_JOBS 下调（如 2），但不会超过 RKVC_BUILD_JOBS_MAX（默认 4）。
#
# 构建目录约定（唯一权威：CMakePresets.json / docs/build-layout.md）:
#   .build/release/     default Release
#   .build/debug/       debug / tidy
#   .build/tests/       tests
#   .build/asan/        asan
#   .build/full-tests/  full-tests
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
    local ver arch
    ver="$(rkvc_project_version)" || return 1
    arch="$(uname -m)"
    printf 'rkvc-%s-linux-%s-portable\n' "$ver" "$arch"
}

rkvc_limit_build_jobs() {
    local max_jobs="${RKVC_BUILD_JOBS_MAX:-4}"
    local jobs="${BUILD_JOBS:-4}"

    if ! [[ "$jobs" =~ ^[0-9]+$ ]] || [ "$jobs" -lt 1 ]; then
        jobs=1
    fi
    if [ "$jobs" -gt "$max_jobs" ]; then
        jobs="$max_jobs"
    fi

    BUILD_JOBS="$jobs"
    export BUILD_JOBS
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
