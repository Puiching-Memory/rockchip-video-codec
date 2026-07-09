#!/bin/bash
# scripts/build-common.sh — 构建脚本共用辅助函数
#
# 用法（在其它构建脚本中）:
#   source "$(dirname "$0")/build-common.sh"
#   rkvc_limit_build_jobs
#   ver="$(rkvc_project_version)"
#
# 可通过环境变量 BUILD_JOBS 下调（如 2），但不会超过 RKVC_BUILD_JOBS_MAX（默认 4）。

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
