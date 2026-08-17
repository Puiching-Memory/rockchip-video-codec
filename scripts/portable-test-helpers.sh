#!/bin/bash
# scripts/portable-test-helpers.sh — 可移植包测试共用辅助函数

# 依赖库清单等共用定义（与本文件同目录）
# shellcheck source=build-common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/build-common.sh"

# GitHub Actions 等无 RKMPP 设备的环境应跳过编解码冒烟；RK3588 实机默认可跑。
# RKVC_RUN_HARDWARE_TESTS=1 且无设备时改为失败（与 CTest 硬件用例 opt-in 一致）。
portable_mpp_device_accessible() {
    local path
    for path in /dev/mpp_service /dev/mpp-service /dev/rkvenc /dev/rkvdec \
                /dev/vpu_service /dev/vpu-service; do
        if [ -r "$path" ] && [ -w "$path" ]; then
            return 0
        fi
    done
    return 1
}

portable_skip_hardware_tests() {
    if portable_mpp_device_accessible; then
        return 1
    fi
    if [ -n "${RKVC_RUN_HARDWARE_TESTS:-}" ] && \
       [ "${RKVC_RUN_HARDWARE_TESTS}" != "0" ]; then
        return 1
    fi
    return 0
}

# NPU 可访问：debugfs 版本节点，或 DRM by-path 下的 npu-render。
portable_npu_accessible() {
    if [ -r /sys/kernel/debug/rknpu/version ]; then
        return 0
    fi
    local path
    for path in /dev/dri/by-path/*npu-render*; do
        if [ -e "$path" ] && [ -r "$path" ] && [ -w "$path" ]; then
            return 0
        fi
    done
    return 1
}

# 与 MPP 硬件段一致：无 NPU 时默认 skip；RKVC_RUN_HARDWARE_TESTS=1 则要求有 NPU。
portable_skip_npu_tests() {
    if portable_npu_accessible; then
        return 1
    fi
    if [ -n "${RKVC_RUN_HARDWARE_TESTS:-}" ] && \
       [ "${RKVC_RUN_HARDWARE_TESTS}" != "0" ]; then
        return 1
    fi
    return 0
}

# 授权版（*-licensed）包在无有效 license 时返回 0：
# 功能类测试会全部报 "license check failed"，应整体跳过以免淹没真实问题。
# 行为探测而非文件存在性判断：license 可能过期或绑定他机，存在≠有效。
portable_license_blocked() {
    local pkg_dir="$1"
    case "$pkg_dir" in
        *-licensed) ;;
        *) return 1 ;;
    esac
    [ -x "$pkg_dir/bin/rkvc_info" ] || return 1
    # licensed 版 --version 触发 license 校验；--json 不触发，不能用于探测
    local out
    if out=$("$pkg_dir/bin/rkvc_info" --version 2>&1); then
        return 1
    fi
    printf '%s' "$out" | grep -qi 'license'
}

generate_raw_nv12() {
    local path="$1" w="$2" h="$3" n="${4:-10}"
    local y_plane=$((w * h))
    local uv_plane=$((y_plane / 2))
    local i

    : > "$path"
    for ((i = 0; i < n; i++)); do
        dd if=/dev/zero bs="$y_plane" count=1 status=none 2>/dev/null >> "$path"
        dd if=/dev/zero bs="$uv_plane" count=1 status=none 2>/dev/null >> "$path"
    done
}

encode_test_clip() {
    local pkg_dir="$1" out="$2" size="$3" frames="${4:-10}" bitrate="${5:-1000000}"
    local example="$pkg_dir/examples/bin/example_encode_file"

    if [[ -x "$example" ]]; then
        env LD_LIBRARY_PATH="$pkg_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$example" -o "$out" -s "$size" -n "$frames" -b "$bitrate"
        return $?
    fi

    local w="${size%x*}" h="${size#*x}"
    local raw="$out.raw.nv12"
    generate_raw_nv12 "$raw" "$w" "$h" "$frames"
    env LD_LIBRARY_PATH="$pkg_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$pkg_dir/bin/rkvc_encode" -i "$raw" -o "$out" -s "$size" -p realtime
}
