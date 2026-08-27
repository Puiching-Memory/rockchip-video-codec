#!/bin/bash
# scripts/package-portable.sh — 从源码构建可移植二进制包
#
# 用法:
#   ./scripts/package-portable.sh [--clean] [--license] \
#       [--platforms rk3576,rk3588,rv1126b] [--sr-weight PATH] [--allow-skip-sr]
#
# 选项:
#   --clean             清理所有中间产物后全量重建
#   --license           开启 1机1码强制授权 (RKVC_ENABLE_LICENSE=ON, 运行时校验)
#   --platforms LIST    逗号分隔目标板列表（如 rk3576,rk3588,rv1126b），每平台产一个包；
#                       默认探测本机 SoC (/proc/device-tree/compatible)
#   --sr-weight PATH    本地 SR 权重 best_ema.pth（缺省自动从 HuggingFace 下载）
#   --mlvc-variants L   MLVC 变体列表: mlvc / mlvc-s / all（默认 mlvc-s，只带轻量版）
#   --allow-skip-sr     SR 权重不可得/导出失败时警告跳过（默认报错）
#   --no-encrypt-models 关闭模型自研加密（默认开启：包内 .rknn 用 rkvc_model_crypt
#                       加密，目标机须持每机签发的 model.key 才能解密运行）
#   --no-rknn           不下载 librknnrt、不启用 NPU 模块、不生产/打包模型（CI 用）
#   --no-model-env-sync 不自动创建/同步模型导出环境（默认完整 NPU 打包自动执行）
#   --no-test           打包后不自动运行包内自测（默认每个平台包产出后自动跑 test.sh，
#                       仅对平台与本机 SoC 匹配的包执行；失败则打包以错误退出）
#   --target-arch ARCH   目标 CPU：native / aarch64（x86 交叉打包使用 aarch64）
#   --cross-prefix PFX  交叉工具前缀（默认 aarch64-linux-gnu-）
#   --sysroot DIR        可选目标 sysroot；Debian multiarch 工具链通常无需指定
#   --qemu-test          交叉打包后用 qemu-aarch64 跑结构检查与用户态 smoke
#   --qemu PATH          QEMU user-mode 程序（默认从 PATH 查找 qemu-aarch64）
#
# 流程:
#   1. 从 third_party/mpp 子模块编译 rockchip-mpp
#   2. 从 third_party/librga 子模块安装预编译 librga
#   3. 从 airockchip/rknn-toolkit2 下载 librknnrt（scripts/install-rknnrt.sh）
#   4. 从 third_party/ffmpeg-rockchip 子模块编译 ffmpeg
#   5. 用编译的 ffmpeg / MPP / librga / librknnrt 构建 rkvc
#   6. 按平台生产模型（scripts/build-models.sh，权重自动下载）
#   7. 每平台一个包: bundle 动态库（含 librga、librknnrt）+ 模型 + RPATH 打包；
#      模型默认经 rkvc_model_crypt 加密（需 --no-encrypt-models 关闭）
#   8. 自动运行包内自测 test.sh（平台与本机匹配时；--no-test 跳过）
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
RKNN_PREFIX="$PROJECT_DIR/.build/deps/rknn-install"
LIBSODIUM_PREFIX="$PROJECT_DIR/.build/deps/libsodium-install"
OUT_DIR="$PROJECT_DIR/.build/dist"

VERSION="$(rkvc_project_version)"
ARCH="$(uname -m)"

# 授权构建模式: 标准 / 强制授权
CLEAN=0
LICENSE=0
PLATFORMS=""
SR_WEIGHT=""
MLVC_VARIANTS="mlvc-s"
ALLOW_SKIP_SR=0
NO_RKNN=0
MODEL_ENV_SYNC=1
ENCRYPT_MODELS=1
RUN_TEST=1
TARGET_ARCH="${RKVC_TARGET_ARCH:-native}"
CROSS_PREFIX="${RKVC_CROSS_PREFIX:-}"
TARGET_SYSROOT="${RKVC_SYSROOT:-}"
QEMU_TEST=0
QEMU_BIN="${RKVC_QEMU:-}"
PKG_SUFFIXES=()
# 本机自测用 model.key 路径（加密模型时由 prepare_model_crypt_keys 签发）
MODEL_KEY_FILE=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean)          CLEAN=1; shift ;;
        --license)        LICENSE=1; shift ;;
        --platforms)      PLATFORMS="$2"; shift 2 ;;
        --sr-weight)      SR_WEIGHT="$2"; shift 2 ;;
        --mlvc-variants)  MLVC_VARIANTS="$2"; shift 2 ;;
        --allow-skip-sr)  ALLOW_SKIP_SR=1; shift ;;
        --no-encrypt-models) ENCRYPT_MODELS=0; shift ;;
        --no-rknn)        NO_RKNN=1; shift ;;
        --no-model-env-sync) MODEL_ENV_SYNC=0; shift ;;
        --no-test)        RUN_TEST=0; shift ;;
        --target-arch)    TARGET_ARCH="$2"; shift 2 ;;
        --cross-prefix)   CROSS_PREFIX="$2"; shift 2 ;;
        --sysroot)        TARGET_SYSROOT="$2"; shift 2 ;;
        --qemu-test)      QEMU_TEST=1; shift ;;
        --qemu)           QEMU_BIN="$2"; shift 2 ;;
        *) echo "错误: 未知参数 '$1'"; exit 2 ;;
    esac
done

TARGET_ARCH="$(rkvc_normalize_target_arch "$TARGET_ARCH")" || exit 2
if [[ "$TARGET_ARCH" == armhf ]]; then
    echo "错误: package-portable.sh 尚未提供 armhf toolchain；当前支持 native / aarch64" >&2
    exit 2
fi
export RKVC_TARGET_ARCH="$TARGET_ARCH"
if [[ "$TARGET_ARCH" == aarch64 ]]; then
    CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
fi
[[ -n "$CROSS_PREFIX" ]] && export RKVC_CROSS_PREFIX="$CROSS_PREFIX"
[[ -n "$TARGET_SYSROOT" ]] && export RKVC_SYSROOT="$TARGET_SYSROOT"

if rkvc_cross_enabled; then
    ARCH="$TARGET_ARCH"
    CROSS_ROOT="$PROJECT_DIR/.build/cross/$TARGET_ARCH"
    MPP_BUILD="$CROSS_ROOT/deps/mpp-build"
    MPP_PREFIX="$CROSS_ROOT/deps/mpp-install"
    RGA_PREFIX="$CROSS_ROOT/deps/librga-install"
    RKNN_PREFIX="$CROSS_ROOT/deps/rknn-install"
    LIBSODIUM_PREFIX="$CROSS_ROOT/deps/libsodium-install"
    SVT_PREFIX="$CROSS_ROOT/deps/svt-av1-install"
    FFMPEG_PREFIX="$CROSS_ROOT/deps/ffmpeg-install"
    OUT_DIR="$CROSS_ROOT/dist"
fi

HOST_LIBSODIUM_PREFIX="$LIBSODIUM_PREFIX"
if rkvc_cross_enabled; then
    HOST_LIBSODIUM_PREFIX="$PROJECT_DIR/.build/host/deps/libsodium-install"
fi

# 展开 MLVC 变体列表（all → mlvc mlvc-s；非法名报错）
mlvc_variant_list() {
    local v out=()
    local parts
    IFS=',' read -r -a parts <<< "$MLVC_VARIANTS"
    for v in "${parts[@]}"; do
        v="${v// /}"
        if [[ "$v" == all ]]; then
            echo "mlvc mlvc-s"
            return 0
        fi
        if [[ "$v" != mlvc && "$v" != mlvc-s ]]; then
            echo "错误: 不支持的 --mlvc-variants 取值 '$v'（可选: mlvc / mlvc-s / all）" >&2
            return 1
        fi
        out+=("$v")
    done
    [[ ${#out[@]} -gt 0 ]] || { echo "错误: --mlvc-variants 为空" >&2; return 1; }
    echo "${out[*]}"
}
VARIANTS_EXPANDED="$(mlvc_variant_list)" || exit 2

# 授权/标准构建目录隔离：--license 与标准包交替打包时避免 CMake 选项翻转触发全量重编。
# 目录与 CMakePresets.json「portable」一致（授权版为其平行目录）；手写 -B 兼容 CMake < 3.21。
RKVC_BUILD="$PROJECT_DIR/.build/portable"
rkvc_cross_enabled && RKVC_BUILD="$PROJECT_DIR/.build/cross/$TARGET_ARCH/portable"
[[ $LICENSE -eq 1 ]] && RKVC_BUILD="$PROJECT_DIR/.build/portable-licensed"
if rkvc_cross_enabled && [[ $LICENSE -eq 1 ]]; then
    RKVC_BUILD="$PROJECT_DIR/.build/cross/$TARGET_ARCH/portable-licensed"
fi

# 与 tools/mlvc/rknn_convert.DEFAULT_PLATFORMS、tools/sr/export_model.py --target 对齐；
# 注：SR 导出的 --target 仅支持 rk3588/rk3576/rv1126b，rk3568/rk3566 只能出 MLVC 模型。
VALID_PLATFORMS="rk3588 rk3576 rk3568 rk3566 rv1126b"

validate_platform() {
    case " $VALID_PLATFORMS " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

platform_supports_sr() {
    case "$1" in
        rk3588|rk3576|rv1126b) return 0 ;;
        *) return 1 ;;
    esac
}

detect_local_soc() {
    local compat soc
    compat="$(tr -d '\0' < /proc/device-tree/compatible 2>/dev/null || true)"
    for soc in rk3588 rk3576 rk3568 rk3566 rv1126b; do
        if [[ "$compat" == *"$soc"* ]]; then
            echo "$soc"
            return 0
        fi
    done
    return 1
}

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
    for cmd in gcc g++ cmake make pkg-config patchelf python3; do
        command -v "$cmd" &>/dev/null || missing+=("$cmd")
    done
    if [[ ${#missing[@]} -gt 0 ]]; then
        echo "错误: 缺少工具: ${missing[*]}"
        exit 1
    fi
    if rkvc_cross_enabled; then
        rkvc_require_cross_tools || exit 1
        if [[ -n "$TARGET_SYSROOT" && ! -d "$TARGET_SYSROOT" ]]; then
            echo "错误: --sysroot 不存在: $TARGET_SYSROOT" >&2
            exit 1
        fi
        if [[ $QEMU_TEST -eq 1 ]]; then
            QEMU_BIN="${QEMU_BIN:-qemu-aarch64}"
            if ! command -v "$QEMU_BIN" >/dev/null 2>&1; then
                echo "错误: --qemu-test 需要 qemu-aarch64（或用 --qemu PATH 指定）" >&2
                exit 1
            fi
        fi
    elif [[ $QEMU_TEST -eq 1 ]]; then
        echo "错误: --qemu-test 仅用于交叉构建" >&2
        exit 2
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
    if [[ $LICENSE -eq 1 || ( $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ) ]]; then
        if [[ ! -f "$PROJECT_DIR/third_party/libsodium/configure.ac" ]]; then
            echo "错误: libsodium 子模块未初始化，运行: git submodule update --init third_party/libsodium"
            exit 1
        fi
        local sod_missing=()
        for cmd in autoreconf automake autoconf libtoolize; do
            command -v "$cmd" >/dev/null || sod_missing+=("$cmd")
        done
        if [[ ${#sod_missing[@]} -gt 0 ]]; then
            echo "错误: 模型加密/授权构建需要 autotools 工具: ${sod_missing[*]} (autoconf/automake/libtool)"
            exit 1
        fi
    fi
}

prepare_model_env() {
    [[ $NO_RKNN -eq 0 ]] || return 0
    if [[ $MODEL_ENV_SYNC -eq 1 ]]; then
        "$SCRIPT_DIR/prepare-model-env.sh"
    elif [[ ! -x "${RKVC_MODEL_PYTHON:-$PROJECT_DIR/.venv/bin/python}" ]]; then
        echo "错误: --no-model-env-sync 已指定，但模型导出 Python 不存在" >&2
        echo "  请运行 ./scripts/prepare-model-env.sh 或设置 RKVC_MODEL_PYTHON" >&2
        return 1
    fi
}

build_mpp() {
    echo "=== 构建 rockchip-mpp ==="

    if [[ $CLEAN -eq 1 ]]; then
        rm -rf "$MPP_BUILD" "$MPP_PREFIX"
    fi

    if [[ -f "$MPP_BUILD/mpp/librockchip_mpp.so" && -f "$MPP_PREFIX/lib/librockchip_mpp.so" \
          && -f "$MPP_PREFIX/.rkvc-complete" ]]; then
        echo "--- rockchip-mpp 已构建，跳过 (用 --clean 重建) ---"
        return
    fi

    local gen
    gen="$(detect_generator "$MPP_BUILD")"
    echo "--- 使用生成器: $gen ---"

    local cmake_cross_args=()
    if rkvc_cross_enabled; then
        cmake_cross_args+=(
            -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchains/aarch64-linux-gnu.cmake"
        )
    fi
    cmake -S "$MPP_SRC" -B "$MPP_BUILD" -G "$gen" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
        -DBUILD_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$MPP_PREFIX" \
        "${cmake_cross_args[@]+"${cmake_cross_args[@]}"}"

    local build_cmd
    build_cmd="$(detect_build_cmd "$MPP_BUILD")"
    $build_cmd -C "$MPP_BUILD" -j"$BUILD_JOBS"
    $build_cmd -C "$MPP_BUILD" install
    # 完成标记：防止磁盘满等中断留下的半成品被误判为已构建
    touch "$MPP_PREFIX/.rkvc-complete"

    echo "--- rockchip-mpp 构建完成 ---"
}

build_svt() {
    # 注意：不能用 ${CLEAN:+--clean}——CLEAN=0 也是非空字符串，会无条件传 --clean，
    # 导致每次打包全量重编（历史 bug，曾使打包缓存全部失效）。
    local args=()
    [[ $CLEAN -eq 1 ]] && args+=(--clean)
    SVT_BUILD="$(dirname "$SVT_PREFIX")/svt-av1-build" \
    SVT_PREFIX="$SVT_PREFIX" \
        "$SCRIPT_DIR/build-svt.sh" "${args[@]+"${args[@]}"}"
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
    PREFIX="$RGA_PREFIX" RKVC_TARGET_ARCH="$TARGET_ARCH" \
        "$SCRIPT_DIR/install-librga.sh"
}

install_rknnrt() {
    if [[ $NO_RKNN -eq 1 ]]; then
        echo "=== 跳过 librknnrt (--no-rknn，包内不含 NPU 功能) ==="
        return 0
    fi
    echo "=== 安装 librknnrt (rknn-toolkit2 → $RKNN_PREFIX) ==="
    if [[ $CLEAN -eq 1 ]]; then
        rm -rf "$RKNN_PREFIX"
    fi
    PREFIX="$RKNN_PREFIX" RKNNRT_ARCH="$TARGET_ARCH" \
        "$SCRIPT_DIR/install-rknnrt.sh"
}

build_libsodium() {
    [[ $LICENSE -eq 1 || ($ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0) ]] || return 0
    echo "=== 构建 libsodium (submodule -> $LIBSODIUM_PREFIX) ==="
    if [[ $CLEAN -eq 1 ]]; then
        rm -rf "$LIBSODIUM_PREFIX"
    fi
    if [[ -f "$LIBSODIUM_PREFIX/lib/libsodium.a" ]] && \
       { ! rkvc_cross_enabled || [[ -f "$HOST_LIBSODIUM_PREFIX/lib/libsodium.a" ]]; }; then
        echo "--- libsodium 已构建，跳过 (用 --clean 重建) ---"
        return
    fi
    if rkvc_cross_enabled; then
        if [[ $CLEAN -eq 1 ]]; then
            rm -rf "$HOST_LIBSODIUM_PREFIX" "$PROJECT_DIR/.build/host/deps/libsodium-build"
        fi
        if [[ ! -f "$HOST_LIBSODIUM_PREFIX/lib/libsodium.a" ]]; then
            env -u RKVC_TARGET_ARCH -u RKVC_CROSS_PREFIX -u RKVC_SYSROOT \
                PREFIX="$HOST_LIBSODIUM_PREFIX" \
                BUILD_DIR="$PROJECT_DIR/.build/host/deps/libsodium-build" \
                "$SCRIPT_DIR/install-libsodium.sh"
        fi
        local cross_cc="${CROSS_PREFIX}gcc"
        PREFIX="$LIBSODIUM_PREFIX" \
        BUILD_DIR="$(dirname "$LIBSODIUM_PREFIX")/libsodium-build" \
        CC="$cross_cc" \
            "$SCRIPT_DIR/install-libsodium.sh"
    else
        PREFIX="$LIBSODIUM_PREFIX" "$SCRIPT_DIR/install-libsodium.sh"
    fi
}

prepare_license_keys() {
    [[ $LICENSE -eq 1 ]] || return 0
    echo "=== 准备授权密钥 ==="
    local pkg_name="${1:-licensed}"

    local keys_dir="$PROJECT_DIR/tools/keys"
    local secret_key="$keys_dir/secret.key"
    local public_key="$keys_dir/public.key"
    local rkvc_lic="$PROJECT_DIR/.build/deps/rkvc_lic"

    # 编译临时 rkvc_lic（仅依赖 libsodium，不走完整 CMake）。
    # 须与 CMakeLists.txt 中 rkvc_lic 目标同源：tools + license_machine。
    # 总是重新编译：rkvc_lic 源码/头文件可能已更新，避免复用旧格式二进制。
    echo "--- 编译 rkvc_lic (host) ---"
    mkdir -p "$(dirname "$rkvc_lic")"
    cc -O2 -o "$rkvc_lic" \
        "$PROJECT_DIR/tools/rkvc_lic.c" \
        "$PROJECT_DIR/lib/license_machine.c" \
        -I"$PROJECT_DIR/lib" \
        -I"$HOST_LIBSODIUM_PREFIX/include" \
        "$HOST_LIBSODIUM_PREFIX/lib/libsodium.a" \
        -lpthread

    # 检查/生成密钥对（首次自动生成，之后复用）
    if [[ ! -f "$secret_key" ]] || [[ ! -f "$public_key" ]]; then
        echo "--- 生成 Ed25519 密钥对 ---"
        mkdir -p "$keys_dir"
        "$rkvc_lic" genkey -o "$keys_dir"
        echo "⚠ 私钥: $secret_key (已 gitignore, 切勿提交/分发)"
    else
        echo "--- 密钥对已存在，复用 ---"
    fi

    # 交叉打包机不是目标 Rockchip，不能生成有意义的本机自测 license。
    if rkvc_cross_enabled; then
        RKVC_LICENSE_FILE=""
        echo "--- 交叉构建: 跳过绑定 x86 打包机的自测 license ---"
        return 0
    fi

    # 签发本机自测 license（绑定打包机 machine-id, 永久有效）
    echo "--- 签发本机自测 license ---"
    local machine_id
    machine_id="$("$rkvc_lic" machine-id)"
    mkdir -p "$OUT_DIR"
    RKVC_LICENSE_FILE="$OUT_DIR/${pkg_name}.lic"
    "$rkvc_lic" issue -m "$machine_id" -k "$secret_key" -o "$RKVC_LICENSE_FILE"
    echo "  license: $RKVC_LICENSE_FILE (本机自测用, 不随包分发)"

    # 自验证
    if "$rkvc_lic" verify -f "$RKVC_LICENSE_FILE" -k "$public_key" >/dev/null 2>&1; then
        echo "  自验证: OK"
    else
        echo "  错误: license 自验证失败"
        return 1
    fi
}

# 模型自研加密：编译工具、准备/生成密钥、签发本机自测 model.key。
# master.key 构建时经 -DRKVC_MODEL_MASTERKEY_FILE 混淆内嵌进 librkvc；
# data.key 用于加密模型并随 model.key 每机签发；两者均不随包分发。
prepare_model_crypt_keys() {
    [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]] || return 0
    echo "=== 准备模型加密密钥 ==="
    local pkg_name="${1:-portable}"

    local keys_dir="$PROJECT_DIR/tools/keys"
    local tool="$PROJECT_DIR/.build/deps/rkvc_model_crypt"

    # 编译临时工具（仅依赖 libsodium + 机器码指纹，不走完整 CMake）。
    # 须与 CMakeLists.txt 中 rkvc_model_crypt 目标同源。
    echo "--- 编译 rkvc_model_crypt (host) ---"
    mkdir -p "$(dirname "$tool")"
    cc -O2 -o "$tool" \
        "$PROJECT_DIR/tools/rkvc_model_crypt.c" \
        "$PROJECT_DIR/lib/license_machine.c" \
        -I"$PROJECT_DIR/lib" \
        -I"$HOST_LIBSODIUM_PREFIX/include" \
        "$HOST_LIBSODIUM_PREFIX/lib/libsodium.a" \
        -lpthread

    mkdir -p "$keys_dir"
    if [[ -f "$keys_dir/master.key" && ! -f "$keys_dir/data.key" ]]; then
        echo "错误: 已有 master.key 但 data.key 缺失；拒绝自动轮换主密钥" >&2
        echo "  请从备份恢复 data.key，或显式移走 master.key 后重新生成整套密钥并重签所有 model.key" >&2
        return 1
    fi
    if [[ ! -f "$keys_dir/master.key" && -f "$keys_dir/data.key" ]]; then
        echo "错误: 已有 data.key 但 master.key 缺失；请从备份恢复 master.key" >&2
        return 1
    fi
    if [[ ! -f "$keys_dir/master.key" ]]; then
        echo "--- 生成模型加密密钥对 (master.key + data.key) ---"
        "$tool" genkey -o "$keys_dir"
        echo "⚠ 密钥位于 $keys_dir/ (已 gitignore, 切勿提交/随包分发)"
    else
        echo "--- 模型加密密钥已存在，复用 ---"
    fi

    if rkvc_cross_enabled; then
        MODEL_KEY_FILE=""
        echo "--- 交叉构建: 跳过绑定 x86 打包机的自测 model.key ---"
        return 0
    fi

    # 签发本机自测 model.key（绑定打包机机器码，不随包分发）
    echo "--- 签发本机自测 model.key ---"
    local machine_id
    machine_id="$("$tool" machine-id)"
    mkdir -p "$OUT_DIR"
    MODEL_KEY_FILE="$OUT_DIR/${pkg_name}.model.key"
    "$tool" issue -d "$keys_dir/data.key" -m "$keys_dir/master.key" \
        -M "$machine_id" -o "$MODEL_KEY_FILE"
    echo "  model.key: $MODEL_KEY_FILE (本机自测用, 不随包分发)"
}

# 包内 .rknn 原地加密（复制进包后执行，不污染生产缓存）
encrypt_package_models() {
    local pkg_dir="$OUT_DIR/$1"
    [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]] || return 0
    local tool="$PROJECT_DIR/.build/deps/rkvc_model_crypt"
    local data_key="$PROJECT_DIR/tools/keys/data.key"
    [[ -x "$tool" && -f "$data_key" ]] || {
        echo "  错误: 模型加密工具/密钥缺失 ($tool / $data_key)"
        return 1
    }
    echo "--- 加密包内模型 (rkvc_model_crypt) ---"
    local m count=0 models_list
    models_list="$(mktemp)"
    if ! find "$pkg_dir/models" -name '*.rknn' -type f -print0 >"$models_list"; then
        rm -f "$models_list"
        echo "  错误: 无法枚举包内 RKNN 模型" >&2
        return 1
    fi
    while IFS= read -r -d '' m; do
        if ! "$tool" encrypt -d "$data_key" -i "$m" >/dev/null; then
            rm -f "$models_list"
            return 1
        fi
        echo "  ${m#"$pkg_dir/"}"
        count=$((count + 1))
    done <"$models_list"
    rm -f "$models_list"
    if [[ $count -eq 0 ]]; then
        echo "  错误: 启用了模型加密，但包内没有 .rknn" >&2
        return 1
    fi

    echo "  共加密 $count 个 .rknn（目标机须持 model.key 解密）"
}

finalize_package_sr_bundle() {
    local pkg_dir="$OUT_DIR/$1"
    local sr_bundle="$pkg_dir/models/rkvc-sr"
    [[ -f "$sr_bundle/sr_export_manifest.json" ]] || return 0

    local args=(--finalize-portable)
    [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]] && args+=(--encrypted)
    python3 "$PROJECT_DIR/tools/sr/verify_bundle.py" \
        "${args[@]}" "$sr_bundle"
}

prune_package_plaintext_models() {
    local pkg_dir="$OUT_DIR/$1" onnx_list
    onnx_list="$(mktemp)"
    if ! find "$pkg_dir/models" -type f -name '*.onnx' -print >"$onnx_list"; then
        rm -f "$onnx_list"
        echo "错误: 无法枚举包内 ONNX" >&2
        return 1
    fi
    if [[ -s "$onnx_list" ]]; then
        echo "--- 移除仅供生产的明文 ONNX ---"
        while IFS= read -r model; do
            echo "  ${model#"$pkg_dir/"}"
            rm -f "$model"
        done <"$onnx_list"
    fi
    rm -f "$onnx_list"
    if find "$pkg_dir/models" -type f -name '*.onnx' -print -quit | grep -q .; then
        echo "错误: 包内仍存在明文 ONNX" >&2
        return 1
    fi
}

build_ffmpeg() {
    echo "=== 构建 ffmpeg-rockchip (AV1 硬解) ==="
    local args=()
    [[ $CLEAN -eq 1 ]] && args+=(--clean)   # 同 build_svt：不能用 ${CLEAN:+--clean}
    RGA_PREFIX="$RGA_PREFIX" MPP_PREFIX="$MPP_PREFIX" SVT_PREFIX="$SVT_PREFIX" \
    FFMPEG_BUILD_DIR="$(dirname "$FFMPEG_PREFIX")/ffmpeg-build" \
        "$SCRIPT_DIR/rebuild-ffmpeg-rkmpp.sh" "${args[@]+"${args[@]}"}" --prefix "$FFMPEG_PREFIX"
}

build_rkvc() {
    echo ""
    echo "=== 构建 rkvc (.build/portable/) ==="
    # 目录与 CMakePresets.json「portable」一致；此处手写 -B，兼容 CMake < 3.21
    [[ $CLEAN -eq 1 ]] && rm -rf "$RKVC_BUILD"

    local gen
    gen="$(detect_generator)"
    echo "--- 使用生成器: $gen ---"

    local cmake_license_args=(
        -DRKVC_ENABLE_LICENSE=OFF
        -DRKVC_ENABLE_MODEL_CRYPT=OFF
    )
    if [[ $LICENSE -eq 1 ]]; then
        cmake_license_args[0]=-DRKVC_ENABLE_LICENSE=ON
        cmake_license_args+=(
            -DRKVC_LICENSE_PUBKEY_FILE="$PROJECT_DIR/tools/keys/public.key"
        )
    fi
    if [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]]; then
        cmake_license_args[1]=-DRKVC_ENABLE_MODEL_CRYPT=ON
        cmake_license_args+=(
            -DRKVC_MODEL_MASTERKEY_FILE="$PROJECT_DIR/tools/keys/master.key"
        )
    fi

    # 无本仓 librknnrt（或 --no-rknn）时关闭 NPU 模块（不查系统 ldconfig，避免链到 /usr 破坏可移植性）
    local cmake_npu_args=()
    if [[ $NO_RKNN -eq 1 || ! -f "$RKNN_PREFIX/lib/librknnrt.so" ]]; then
        cmake_npu_args+=(
            -DRKVC_ENABLE_RKNN=OFF
            -DRKVC_ENABLE_MLVC=OFF
        )
    else
        # Always set both sides explicitly: this build directory is shared by
        # full and --no-rknn packaging, so cached OFF values must not leak into
        # a later full package.
        cmake_npu_args+=(
            -DRKVC_ENABLE_RKNN=ON
            -DRKVC_ENABLE_MLVC=ON
        )
    fi

    local cmake_cross_args=()
    if rkvc_cross_enabled; then
        cmake_cross_args+=(
            -DCMAKE_TOOLCHAIN_FILE="$PROJECT_DIR/cmake/toolchains/aarch64-linux-gnu.cmake"
        )
    fi

    cmake -B "$RKVC_BUILD" -G "$gen" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_BUILD_PARALLEL_LEVEL="$BUILD_JOBS" \
        -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=ON \
        -DMPP_BUILD_DIR="$MPP_BUILD" \
        -DMPP_INSTALL_PREFIX="$MPP_PREFIX" \
        -DSVT_PREFIX="$SVT_PREFIX" \
        -DFFMPEG_PREFIX="$FFMPEG_PREFIX" \
        -DRGA_PREFIX="$RGA_PREFIX" \
        -DRKNN_PREFIX="$RKNN_PREFIX" \
        -DLIBSODIUM_PREFIX="$LIBSODIUM_PREFIX" \
        "${cmake_license_args[@]+"${cmake_license_args[@]}"}" \
        "${cmake_npu_args[@]+"${cmake_npu_args[@]}"}" \
        "${cmake_cross_args[@]+"${cmake_cross_args[@]}"}" \
        "$PROJECT_DIR"

    local build_cmd
    build_cmd="$(detect_build_cmd)"
    $build_cmd -C "$RKVC_BUILD" -j"$BUILD_JOBS"
    echo "--- rkvc 构建完成 ---"
}

package_one() {
    local platform="$1"
    local models_root="$PROJECT_DIR/.build/models/$platform"
    PKG_NAME="$(rkvc_portable_pkg_dir "$platform")"
    [[ $NO_RKNN -eq 1 ]] && PKG_NAME="${PKG_NAME}-no-npu"
    [[ $LICENSE -eq 1 ]] && PKG_NAME="${PKG_NAME}-licensed"
    PKG_SUFFIXES+=("$PKG_NAME")
    echo ""
    echo "=== 打包 $PKG_NAME ==="

    rm -rf "$OUT_DIR/$PKG_NAME"
    mkdir -p "$OUT_DIR/$PKG_NAME"/{bin,lib,include/rkvc,share/pkgconfig,share/rkvc,examples/bin,examples/src,models}

    local package_tools=(
        rkvc_encode rkvc_decode rkvc_info rkvc_bench rkvc_transcode
        rkvc_session_upscale rkvc_yuv_upscale
    )
    [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]] && \
        package_tools+=(rkvc_model_id)
    for tool in "${package_tools[@]}"; do
        [[ -f "$RKVC_BUILD/$tool" ]] && cp "$RKVC_BUILD/$tool" "$OUT_DIR/$PKG_NAME/bin/"
    done

    if [[ $LICENSE -eq 1 && -f "$RKVC_BUILD/rkvc_lic_client" ]]; then
        cp "$RKVC_BUILD/rkvc_lic_client" "$OUT_DIR/$PKG_NAME/bin/rkvc_lic"
        echo "  rkvc_lic  (机器码采集/授权校验工具，不含签发能力)"
    fi

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
    # 清单见 build-common.sh RKVC_BUNDLED_FFMPEG_LIBS
    for name in "${RKVC_BUNDLED_FFMPEG_LIBS[@]}"; do
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
            "$RKNN_PREFIX/lib/librknnrt.so" \
            "$PROJECT_DIR/.build/deps/rknn-install/lib/librknnrt.so"; do
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

        echo "--- 复制 Phase-RLFN SR bundle (models/rkvc-sr/) ---"
        local sr_bundle="$models_root/rkvc-sr"
        local sr_model="$sr_bundle/phase_rlfn_sr_x3.rknn"
        if ! platform_supports_sr "$platform"; then
            echo "  跳过: $platform 不支持 Phase-RLFN SR target（仍打包 MLVC）"
        elif [[ -f "$sr_model" && -f "$sr_bundle/sr_export_manifest.json" && \
                -f "$sr_bundle/LICENSE.rknn-super-resolution-MIT" ]] && \
             python3 "$PROJECT_DIR/tools/sr/verify_bundle.py" "$sr_bundle"; then
            cp -a "$sr_bundle" "$OUT_DIR/$PKG_NAME/models/"
            echo "  models/rkvc-sr/  (Phase-RLFN runtime bundle)"
        elif [[ $ALLOW_SKIP_SR -eq 1 ]]; then
            echo "  警告: Phase-RLFN bundle 不完整，跳过: $sr_bundle (--allow-skip-sr)"
        else
            echo "  错误: 已打包 librknnrt，但 Phase-RLFN bundle 不完整: $sr_bundle"
            echo "  可传 --allow-skip-sr 跳过，或用 --sr-weight 提供本地权重"
            return 1
        fi
        # MLVC 神经视频编解码 bundle：按 --mlvc-variants 选择（默认仅 mlvc-s），各含
        # RKNN + PMF + QP 补丁 + 导出 manifest，须整包随分发，不能跨变体混用。
        local variant variant_dir
        for variant in $VARIANTS_EXPANDED; do
            variant_dir="$models_root/$variant"
            if [[ -d "$variant_dir" ]]; then
                cp -a "$variant_dir" "$OUT_DIR/$PKG_NAME/models/"
                echo "  models/$variant/  (MLVC $variant bundle)"
            else
                echo "  错误: 未找到 MLVC bundle: $variant_dir（运行 ./scripts/build-models.sh --platform $platform --variants $variant）"
                return 1
            fi
        done

        prune_package_plaintext_models "$PKG_NAME"
        encrypt_package_models "$PKG_NAME"
        finalize_package_sr_bundle "$PKG_NAME"
    else
        echo "  跳过 (本构建未启用 RKNN / librkvc 未链接 librknnrt)"
        echo "--- 跳过 Phase-RLFN SR bundle (未链接 librknnrt) ---"
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
License: AGPL-3.0-or-later
Libs: -L\${libdir} -lrkvc
Cflags: -I\${includedir}
EOF

    echo "--- 复制许可证文本 (AGPLv3 + 第三方) ---"
    mkdir -p "$OUT_DIR/$PKG_NAME/licenses"
    cp "$PROJECT_DIR/LICENSE" "$OUT_DIR/$PKG_NAME/licenses/AGPL-3.0.txt"
    echo "  AGPL-3.0.txt"
    local src_lic
    for src_lic in \
        "$PROJECT_DIR/third_party/ffmpeg-rockchip/COPYING.LGPLv3" \
        "$PROJECT_DIR/third_party/SVT-AV1/LICENSE.md" \
        "$PROJECT_DIR/third_party/SVT-AV1/PATENTS.md" \
        "$PROJECT_DIR/third_party/librga/COPYING" \
        "$PROJECT_DIR/third_party/libsodium/LICENSE"; do
        if [[ -f "$src_lic" ]]; then
            cp "$src_lic" "$OUT_DIR/$PKG_NAME/licenses/$(basename "$src_lic")"
            echo "  $(basename "$src_lic")"
        fi
    done
    if [[ -d "$PROJECT_DIR/third_party/mpp/LICENSES" ]]; then
        cp -r "$PROJECT_DIR/third_party/mpp/LICENSES" "$OUT_DIR/$PKG_NAME/licenses/mpp-LICENSES"
        echo "  mpp-LICENSES/"
    fi

    # LGPLv3 §4: 随包提供对 ffmpeg-rockchip 的修改（补丁）与对应源码获取说明
    if ls "$PROJECT_DIR"/patches/ffmpeg-rockchip/*.patch &>/dev/null; then
        local ffmpeg_dir="$OUT_DIR/$PKG_NAME/licenses/ffmpeg-modifications"
        mkdir -p "$ffmpeg_dir"
        cp "$PROJECT_DIR"/patches/ffmpeg-rockchip/*.patch "$ffmpeg_dir/"
        local ffmpeg_commit
        ffmpeg_commit="$(git -C "$FFMPEG_SRC" rev-parse HEAD 2>/dev/null || echo "unknown")"
        {
            echo "ffmpeg-rockchip 修改说明（LGPLv3 第 4 节：修改版本的对应源码）"
            echo "============================================================"
            echo "上游源码: https://github.com/nyanmisaka/ffmpeg-rockchip (branch 8.1)"
            echo "子模块 commit: $ffmpeg_commit"
            echo ""
            echo "修改的对应源码 = 上游源码 + 本目录补丁（按文件名顺序 git apply 应用）："
            for p in "$ffmpeg_dir"/*.patch; do
                echo "  - $(basename "$p")"
            done
            echo ""
            echo "构建 configure 参数: --enable-version3 --enable-rkmpp --enable-rkrga"
            echo "                     --enable-libdrm --enable-libsvtav1 --enable-shared"
        } > "$ffmpeg_dir/MODIFICATIONS.txt"
        echo "  ffmpeg-modifications/ (补丁 + 对应源码说明)"
    fi

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
    cp "$PROJECT_DIR/scripts/build-common.sh" "$OUT_DIR/$PKG_NAME/build-common.sh"
    echo "  build-common.sh (portable-test-helpers.sh 依赖)"
    cp "$PROJECT_DIR/scripts/portable-test-helpers.sh" "$OUT_DIR/$PKG_NAME/portable-test-helpers.sh"
    echo "  portable-test-helpers.sh"
    cp "$PROJECT_DIR/scripts/network-e2e-test.sh" "$OUT_DIR/$PKG_NAME/network-e2e-test.sh"
    chmod +x "$OUT_DIR/$PKG_NAME/network-e2e-test.sh"
    echo "  network-e2e-test.sh"
    cp "$PROJECT_DIR/tools/sr/verify_bundle.py" \
        "$OUT_DIR/$PKG_NAME/share/rkvc/verify_sr_bundle.py"
    echo "  share/rkvc/verify_sr_bundle.py"
    cp "$PROJECT_DIR/scripts/check-elf-deps.py" \
        "$OUT_DIR/$PKG_NAME/share/rkvc/check_elf_deps.py"
    echo "  share/rkvc/check_elf_deps.py"

    cd "$OUT_DIR"
    tar czf "$PKG_NAME.tar.gz" "$PKG_NAME"
    echo "  产物: $OUT_DIR/$PKG_NAME.tar.gz ($(du -h "$PKG_NAME.tar.gz" | cut -f1))"

    echo "--- 验证自包含库 ---"
    if rkvc_cross_enabled; then
        local elf_args=(
            "$OUT_DIR/$PKG_NAME"
            --target "$TARGET_ARCH"
            --require-bundled librkvc.so
            --require-bundled libavcodec.so
            --require-bundled libavformat.so
            --require-bundled libavutil.so
            --require-bundled libswscale.so
            --require-bundled libSvtAv1Enc.so
            --require-bundled librockchip_mpp.so
            --require-bundled librga.so
        )
        [[ -n "$TARGET_SYSROOT" ]] && elf_args+=(--sysroot "$TARGET_SYSROOT")
        [[ -f "$OUT_DIR/$PKG_NAME/lib/librknnrt.so" ]] && \
            elf_args+=(--require-bundled librknnrt.so)
        python3 "$PROJECT_DIR/scripts/check-elf-deps.py" "${elf_args[@]}" || return 1
        echo "--- 目标板前置依赖: glibc/libdrm + Rockchip 内核驱动 ---"
        return 0
    fi

    local unresolved=0
    local wrong_origin=0
    local ldd_output
    if ! ldd_output="$(LD_LIBRARY_PATH="$OUT_DIR/$PKG_NAME/lib" \
        ldd "$OUT_DIR/$PKG_NAME/bin/rkvc_info" 2>&1)"; then
        echo "  错误: 无法检查 rkvc_info 动态依赖" >&2
        echo "$ldd_output" | sed 's/^/    /' >&2
        return 1
    fi

    while read -r line; do
        if echo "$line" | grep -q "not found"; then
            echo "  错误: $line"
            unresolved=1
        fi
    done <<< "$ldd_output"

    for lib in "${RKVC_BUNDLED_ALL_LIBS[@]}"; do
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
    echo "  NPU 驱动/固件     (Phase-RLFN 超分; librknnrt + models/rkvc-sr/ 已随包携带)"
    echo "  /dev/rga          (RGA 设备节点; librga 已随包携带)"
    echo ""
    echo "  安装示例: sudo apt install libdrm-dev"
}

# 打包后自动运行包内自测。仅对平台与本机 SoC 匹配的包执行：
# 非匹配平台上硬件项（NPU 冒烟/编解码）会假失败，须到目标板手动跑。
test_package() {
    local platform="$1" pkg_name="$2"
    [[ $RUN_TEST -eq 1 ]] || return 0

    if rkvc_cross_enabled; then
        if [[ $QEMU_TEST -eq 0 ]]; then
            echo "=== 交叉构建: ELF 静态检查已完成；未启用 --qemu-test ==="
            return 0
        fi
        local cross_args=("$OUT_DIR/$pkg_name" --target "$TARGET_ARCH" --qemu "$QEMU_BIN")
        [[ -n "$TARGET_SYSROOT" ]] && cross_args+=(--sysroot "$TARGET_SYSROOT")
        "$SCRIPT_DIR/test-portable-cross.sh" "${cross_args[@]}"
        return
    fi

    local local_soc
    local_soc="$(detect_local_soc || true)"
    if [[ "$platform" != "$local_soc" ]]; then
        echo ""
        echo "=== 跳过自动自测: 包平台 $platform 与本机 SoC '${local_soc:-未知}' 不匹配 ==="
        echo "  请到目标板解包 $OUT_DIR/$pkg_name.tar.gz 后运行 ./test.sh"
        return 0
    fi

    echo ""
    echo "=== 自动自测: $pkg_name/test.sh ==="
    local log="$OUT_DIR/$pkg_name.test.log"
    local status=0
    # 加密模型包：注入本机自测 model.key（绑定打包机机器码）
    local test_env=()
    [[ -n "$MODEL_KEY_FILE" && -f "$MODEL_KEY_FILE" ]] &&
        test_env+=(RKVC_MODEL_KEY_FILE="$MODEL_KEY_FILE")
    env "${test_env[@]+"${test_env[@]}"}" "$OUT_DIR/$pkg_name/test.sh" 2>&1 | tee "$log" || status=$?
    if [[ $status -ne 0 ]]; then
        echo ""
        echo "错误: 包内自测失败 (完整日志: $log)"
        return 1
    fi
    echo "--- 包内自测通过 (日志: $log) ---"
}

main() {
    local mode="标准版"
    [[ $LICENSE -eq 1 ]] && mode="强制授权版 (运行时校验)"
    echo "=== 可移植包构建 (rkvc $VERSION, $ARCH, $mode) ==="

    # 解析目标平台列表：显式 --platforms 优先，否则探测本机 SoC
    local platforms=()
    if [[ -n "$PLATFORMS" ]]; then
        IFS=',' read -r -a platforms <<< "$PLATFORMS"
        local p
        for p in "${platforms[@]}"; do
            p="${p// /}"
            if ! validate_platform "$p"; then
                echo "错误: 不支持的平台 '$p'（可选: $VALID_PLATFORMS）"
                exit 2
            fi
        done
    else
        local soc
        if soc="$(detect_local_soc)"; then
            platforms=("$soc")
            echo "--- 探测本机 SoC: $soc（可用 --platforms rk3576,rk3588,rv1126b 覆盖）---"
        else
            echo "错误: 无法探测本机 SoC，请用 --platforms 显式指定（可选: $VALID_PLATFORMS）"
            exit 2
        fi
    fi

    check_deps
    prepare_model_env
    build_mpp
    build_svt
    install_rga
    install_rknnrt
    build_libsodium
    local lic_pkg_name
    lic_pkg_name="$(rkvc_portable_pkg_dir "${platforms[0]}")"
    [[ $LICENSE -eq 1 ]] && lic_pkg_name="${lic_pkg_name}-licensed"
    prepare_license_keys "$lic_pkg_name"
    prepare_model_crypt_keys "$lic_pkg_name"
    build_ffmpeg
    build_rkvc

    local plat
    for plat in "${platforms[@]}"; do
        plat="${plat// /}"
        [[ -n "$plat" ]] || continue
        if [[ $NO_RKNN -eq 0 && -f "$RKNN_PREFIX/lib/librknnrt.so" ]]; then
            local bm_args=(--platform "$plat" --variants "$MLVC_VARIANTS")
            [[ -n "$SR_WEIGHT" ]] && bm_args+=(--sr-weight "$SR_WEIGHT")
            [[ $ALLOW_SKIP_SR -eq 1 ]] && bm_args+=(--allow-skip-sr)
            [[ $CLEAN -eq 1 ]] && bm_args+=(--clean)
            "$SCRIPT_DIR/build-models.sh" "${bm_args[@]}"
        else
            echo "--- 跳过模型生产 (未安装 librknnrt，构建不含 RKNN) ---"
        fi
        package_one "$plat"
        test_package "$plat" "$PKG_NAME"
    done

    echo ""
    echo "=== 完成: ${#PKG_SUFFIXES[@]} 个可移植包 ==="
    local suffix
    for suffix in "${PKG_SUFFIXES[@]}"; do
        echo "  $OUT_DIR/$suffix.tar.gz"
    done
    if [[ $LICENSE -eq 1 ]]; then
        echo ""
        echo "⚠ 强制授权版: 目标机须放置有效 license 文件后方可运行"
        echo "  本机自测: RKVC_LICENSE_FILE=\"$RKVC_LICENSE_FILE\" \\"
        echo "            $OUT_DIR/$PKG_NAME/bin/rkvc_info --version"
        echo ""
        echo "  客户签发流程:"
        echo "    1. 客户机运行: rkvc_lic machine-id"
        echo "    2. 打包方签发: rkvc_lic issue -m <客户机器码> -k tools/keys/secret.key -o customer.lic"
        echo "    3. 客户放置:   ~/.config/rkvc/license.lic 或设置 RKVC_LICENSE_FILE"
    fi
    if [[ $ENCRYPT_MODELS -eq 1 && $NO_RKNN -eq 0 ]]; then
        echo ""
        echo "⚠ 模型已加密: 目标机须持每机签发的 model.key 才能加载模型"
        if [[ -n "$MODEL_KEY_FILE" ]]; then
            echo "  本机自测: RKVC_MODEL_KEY_FILE=\"$MODEL_KEY_FILE\" ./test.sh"
        fi
        echo ""
        echo "  客户签发流程:"
        echo "    1. 客户机运行: bin/rkvc_model_id"
        echo "    2. 打包方签发: rkvc_model_crypt issue -d tools/keys/data.key \\"
        echo "                    -m tools/keys/master.key -M <客户机器码> -o model.key"
        echo "    3. 客户放置:   ~/.config/rkvc/model.key 或设置 RKVC_MODEL_KEY_FILE"
    fi
}

main
