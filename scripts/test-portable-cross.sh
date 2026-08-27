#!/bin/bash
# Cross-package checks on an x86 host: static package inspection plus a small
# QEMU user-mode smoke suite. Rockchip VPU/RGA/NPU ioctls are intentionally not
# treated as emulated hardware coverage.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_DIR=""
TARGET="aarch64"
QEMU_BIN="qemu-aarch64"
SYSROOT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --target) TARGET="$2"; shift 2 ;;
        --qemu) QEMU_BIN="$2"; shift 2 ;;
        --sysroot) SYSROOT="$2"; shift 2 ;;
        -h|--help)
            echo "用法: $0 PACKAGE --target aarch64 [--qemu qemu-aarch64] [--sysroot DIR]"
            exit 0
            ;;
        --*) echo "未知参数: $1" >&2; exit 2 ;;
        *) [[ -z "$PKG_DIR" ]] || { echo "只能指定一个 PACKAGE" >&2; exit 2; }; PKG_DIR="$1"; shift ;;
    esac
done

[[ -n "$PKG_DIR" && -d "$PKG_DIR" ]] || { echo "错误: PACKAGE 不存在: $PKG_DIR" >&2; exit 2; }
command -v "$QEMU_BIN" >/dev/null 2>&1 || { echo "错误: 找不到 QEMU: $QEMU_BIN" >&2; exit 1; }

if [[ -z "$SYSROOT" ]]; then
    case "$TARGET" in
        aarch64)
            [[ -e /usr/aarch64-linux-gnu/lib/ld-linux-aarch64.so.1 ]] && \
                SYSROOT=/usr/aarch64-linux-gnu
            ;;
        armhf)
            [[ -e /usr/arm-linux-gnueabihf/lib/ld-linux-armhf.so.3 ]] && \
                SYSROOT=/usr/arm-linux-gnueabihf
            ;;
    esac
fi
[[ -n "$SYSROOT" && -d "$SYSROOT" ]] || {
    echo "错误: QEMU 需要目标 sysroot（用 --sysroot 指定）" >&2
    exit 1
}

inspect_args=(--inspect-only --target "$TARGET")
[[ -n "$SYSROOT" ]] && inspect_args+=(--sysroot "$SYSROOT")
"$SCRIPT_DIR/test-portable.sh" "${inspect_args[@]}" "$PKG_DIR"

case "$TARGET" in
    aarch64) TARGET_TRIPLET=aarch64-linux-gnu ;;
    armhf) TARGET_TRIPLET=arm-linux-gnueabihf ;;
esac
TARGET_LD_LIBRARY_PATH="$PKG_DIR/lib:$SYSROOT/lib:$SYSROOT/usr/lib:$SYSROOT/lib/$TARGET_TRIPLET:$SYSROOT/usr/lib/$TARGET_TRIPLET:/lib/$TARGET_TRIPLET:/usr/lib/$TARGET_TRIPLET"

qemu_run() {
    "$QEMU_BIN" -L "$SYSROOT" \
        -E LD_LIBRARY_PATH="$TARGET_LD_LIBRARY_PATH" \
        "$@"
}

echo ""
echo "--- QEMU $TARGET 用户态 smoke ---"
json="$(qemu_run "$PKG_DIR/bin/rkvc_info" --json 2>&1)" || {
    echo "错误: QEMU 下 rkvc_info --json 失败" >&2
    printf '%s\n' "$json" >&2
    exit 1
}
printf '%s\n' "$json" | grep -q '"version"' || {
    echo "错误: rkvc_info --json 缺少 version" >&2
    exit 1
}
echo "  OK: rkvc_info --json"

if [[ "$PKG_DIR" == *-licensed ]]; then
    echo "  SKIP: licensed 包 --version（交叉构建不签发 x86 自测 license）"
else
    version="$(qemu_run "$PKG_DIR/bin/rkvc_info" --version 2>&1)" || {
        echo "错误: QEMU 下 rkvc_info --version 失败" >&2
        printf '%s\n' "$version" >&2
        exit 1
    }
    printf '%s\n' "$version" | grep -q '^rkvc ' || {
        echo "错误: rkvc_info --version 输出异常: $version" >&2
        exit 1
    }
    echo "  OK: rkvc_info --version"
fi

set +e
negative="$(qemu_run "$PKG_DIR/bin/rkvc_encode" 2>&1)"
negative_status=$?
set -e
if [[ $negative_status -eq 0 ]] || ! printf '%s\n' "$negative" | grep -Eqi 'usage|raw\.nv12'; then
    echo "错误: rkvc_encode 参数负向测试未按预期失败" >&2
    printf '%s\n' "$negative" >&2
    exit 1
fi
echo "  OK: rkvc_encode 参数负向测试"

if [[ -x "$PKG_DIR/bin/rkvc_model_id" ]]; then
    machine="$($QEMU_BIN -L "$SYSROOT" \
        -E LD_LIBRARY_PATH="$TARGET_LD_LIBRARY_PATH" \
        -E RKVC_LICENSE_ALLOW_CONTAINER_MAC=1 \
        "$PKG_DIR/bin/rkvc_model_id" 2>/dev/null)" || true
    if [[ "$machine" =~ ^[0-9a-f]{64}$ ]]; then
        echo "  OK: rkvc_model_id（容器 MAC 测试回退）"
    else
        echo "  SKIP: rkvc_model_id 无可用测试指纹"
    fi
fi

echo "  SKIP: MPP/RGA/RKNN 硬件路径（QEMU user-mode 不模拟 Rockchip 设备）"
echo "QEMU cross-package smoke: OK"
