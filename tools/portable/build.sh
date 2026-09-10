#!/bin/bash
# 可移植包构建：交叉编译 aarch64 CLI + 插件 + 随包运行库，组装、审计、自测后
# 产出 rkvc-<版本>-linux-aarch64-portable.tar.gz(.sha256)。
#
# 构建环境是 tools/portable/Dockerfile 的镜像（jammy + aarch64-linux-gnu-*）；
# 宿主机有 docker 时脚本自动用该镜像重入，已在等价环境里则原样执行。
#
# 用法: tools/portable/build.sh [选项]
#   --jobs N      并行度（默认 nproc）
#   --clean       清空 .build/portable 后全量重建
#   --no-rknn     关闭 NPU 后端（mlvc/sr 落 stub，跳过 librknnrt 下载）
#   --no-av1      不构建 av1 插件（跳过 SVT-AV1 交叉构建）
#   --rknn-dir D  用 D 下 rknn_api.h + librknnrt.so 替代固定版本下载
#   --models D    把 D 下 *.rkmodel 收进包内 models/
#   --out D       产物目录（默认 .build/dist）
set -euo pipefail

# RKNN 运行库固定 2.3.2（与 pyproject.toml 的 rknn-toolkit2 同版本），下载后
# 强制 SHA-256 校验：上游换文件不会静默进包。
RKNN_VERSION=2.3.2
RKNN_URL="https://github.com/airockchip/rknn-toolkit2/raw/v$RKNN_VERSION/rknpu2/runtime/Linux/librknn_api"
RKNN_SO_SHA=d31fc19c85b85f6091b2bd0f6af9d962d5264a4e410bfb536402ec92bac738e8
RKNN_H_SHA=c48e11a6f41b451a5fd1e4ad774ea60252d3d94f78bee9b21ea3d21b21deba9a

# 目标机自带、不进包的运行库；其余 NEEDED 必须在已构建前缀里找到。
# （数组而非空格串：跨行赋值会用换行代替空格，case 模式匹配会失效。）
SYSTEM_LIBS=(libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1
    libstdc++.so.6 libgcc_s.so.1 ld-linux-aarch64.so.1)

is_system_lib() {
    local l
    for l in "${SYSTEM_LIBS[@]}"; do
        [[ "$l" == "$1" ]] && return 0
    done
    return 1
}

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TOOLCHAIN="$SCRIPT_DIR/aarch64-portable.cmake"

usage() {
    cat <<'EOF'
用法: tools/portable/build.sh [选项]
  --jobs N      并行度（默认 nproc）
  --clean       清空 .build/portable 后全量重建
  --no-rknn     关闭 NPU 后端（mlvc/sr 落 stub，跳过 librknnrt 下载）
  --no-av1      不构建 av1 插件（跳过 SVT-AV1 交叉构建）
  --rknn-dir D  用 D 下 rknn_api.h + librknnrt.so 替代固定版本下载
  --models D    把 D 下 *.rkmodel 收进包内 models/
  --out D       产物目录（默认 .build/dist）
EOF
}

if [[ -z "${RKVC_PORTABLE_IN_CONTAINER:-}" ]]; then
    # 宿主机入口：有 docker 就重入固定镜像——宿主自带的交叉工具链可能是
    # noble 的 gcc-13（目标 glibc 2.39），会顶穿 2.34 基线；没有 docker 才
    # 直接用现场工具链（等价环境，如镜像内手工执行），基线由审计兜底。
    if command -v docker >/dev/null 2>&1; then
        # 无直连环境（内网机）里构建容器同样需要代理：标准代理变量设置了就
        # 转发，未设置则什么都不加。镜像内 apt 会用 chsrc 换源后直连镜像站。
        build_args=()
        run_env=(--network=host)
        for v in http_proxy https_proxy all_proxy no_proxy \
            HTTP_PROXY HTTPS_PROXY ALL_PROXY NO_PROXY; do
            if [[ -n "${!v:-}" ]]; then
                build_args+=(--build-arg "$v=${!v}")
                run_env+=(-e "$v=${!v}")
            fi
        done
        if [[ -n "${RKVC_CHSRC_MIRROR:-}" ]]; then
            build_args+=(--build-arg "CHSRC_MIRROR=$RKVC_CHSRC_MIRROR")
        fi
        # --network=host：镜像内 apt 与容器内 curl 都要能碰到宿主 loopback
        # 上的代理（内网机代理通常只监听 127.0.0.1）。
        docker build -q --network=host "${build_args[@]}" \
            -t rkvc-portable-cross "$SCRIPT_DIR" >/dev/null
        exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
            -e RKVC_PORTABLE_IN_CONTAINER=1 "${run_env[@]}" \
            -v "$REPO_ROOT:$REPO_ROOT" -w "$REPO_ROOT" \
            rkvc-portable-cross "$SCRIPT_DIR/build.sh" "$@"
    fi
    if ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1; then
        echo "错误: 需要 docker，或直接在 tools/portable/Dockerfile 镜像内运行" >&2
        exit 2
    fi
fi

jobs="$(nproc)"
clean=0
with_rknn=1
with_av1=1
rknn_dir=""
models_dir=""
out_dir=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --jobs) jobs="${2:?--jobs 需要参数}"; shift 2 ;;
        --clean) clean=1; shift ;;
        --no-rknn) with_rknn=0; shift ;;
        --no-av1) with_av1=0; shift ;;
        --rknn-dir) rknn_dir="${2:?--rknn-dir 需要参数}"; shift 2 ;;
        --models) models_dir="${2:?--models 需要参数}"; shift 2 ;;
        --out) out_dir="${2:?--out 需要参数}"; shift 2 ;;
        -h | --help) usage; exit 0 ;;
        *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    esac
done

build="$REPO_ROOT/.build/portable"
deps="$build/deps"
stage="$build/pkg"
logs="$build/logs"
version="$(sed -n 's/^project(rkvc VERSION \([0-9][0-9.]*\).*/\1/p' \
    "$REPO_ROOT/CMakeLists.txt")"
if [[ -z "$version" ]]; then
    echo "错误: 无法从 CMakeLists.txt 解析版本号" >&2
    exit 2
fi
pkg_name="rkvc-$version-linux-aarch64-portable"
pkg="$stage/$pkg_name"
if [[ -z "$out_dir" ]]; then
    out_dir="$REPO_ROOT/.build/dist"
fi

if ((clean)); then
    rm -rf "$build"
fi
mkdir -p "$deps" "$stage" "$logs" "$out_dir"

run_logged() { # <日志> <描述> <命令...>
    local log="$1" what="$2"
    shift 2
    : >"$log"
    if ! "$@" >>"$log" 2>&1; then
        echo "错误: $what 失败，日志尾部（$log）:" >&2
        tail -n 20 "$log" >&2
        exit 1
    fi
}

submodule_commit() {
    git -C "$REPO_ROOT/third_party/$1" rev-parse HEAD 2>/dev/null || echo unknown
}

sha256_of() {
    if [[ -f "$1" ]]; then
        sha256sum "$1" | cut -d' ' -f1
    else
        echo missing
    fi
}

build_mpp() {
    local prefix="$deps/mpp" commit
    commit="$(submodule_commit mpp)"
    if [[ -f "$prefix/lib/librockchip_mpp.so.1" ]] &&
        [[ "$(cat "$prefix/.rkvc-stamp" 2>/dev/null)" == "$commit" ]]; then
        echo "-- mpp: 缓存命中（$commit）"
        return
    fi
    echo "-- mpp: 交叉构建（$commit）"
    run_logged "$logs/mpp.log" "mpp 配置" cmake \
        -S "$REPO_ROOT/third_party/mpp" -B "$deps/mpp-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TEST=OFF -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_INSTALL_PREFIX="$prefix"
    run_logged "$logs/mpp.log" "mpp 构建" cmake --build "$deps/mpp-build" \
        -j "$jobs"
    run_logged "$logs/mpp.log" "mpp 安装" cmake --install "$deps/mpp-build"
    echo "$commit" >"$prefix/.rkvc-stamp"
}

build_svt() {
    local prefix="$deps/svt" commit
    commit="$(submodule_commit SVT-AV1)"
    if [[ -f "$prefix/lib/libSvtAv1Enc.so" ]] &&
        [[ "$(cat "$prefix/.rkvc-stamp" 2>/dev/null)" == "$commit" ]]; then
        echo "-- svt: 缓存命中（$commit）"
        return
    fi
    echo "-- svt: 交叉构建（$commit）"
    # SVT_AV1_LTO=OFF：GCC≥9 默认开 LTO，CMake 会生成裸 -flto，链接阶段
    # 退化为单线程，RC 构建从分钟级涨到十几分钟。
    run_logged "$logs/svt.log" "SVT-AV1 配置" cmake \
        -S "$REPO_ROOT/third_party/SVT-AV1" -B "$deps/svt-build" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DSVT_AV1_LTO=OFF \
        -DCMAKE_INSTALL_PREFIX="$prefix"
    run_logged "$logs/svt.log" "SVT-AV1 构建" cmake --build "$deps/svt-build" \
        -j "$jobs"
    run_logged "$logs/svt.log" "SVT-AV1 安装" cmake --install "$deps/svt-build"
    echo "$commit" >"$prefix/.rkvc-stamp"
}

prepare_rknn() {
    local prefix="$deps/rknn"
    local so="$prefix/lib/librknnrt.so" hdr="$prefix/include/rknn_api.h"
    if [[ "$(sha256_of "$so")" == "$RKNN_SO_SHA" ]] &&
        [[ "$(sha256_of "$hdr")" == "$RKNN_H_SHA" ]]; then
        echo "-- rknn: 缓存命中（$RKNN_VERSION）"
        return
    fi
    mkdir -p "$prefix/lib" "$prefix/include"
    if [[ -n "$rknn_dir" ]]; then
        echo "-- rknn: 使用 $rknn_dir"
        cp -f "$rknn_dir/librknnrt.so" "$so"
        cp -f "$rknn_dir/rknn_api.h" "$hdr"
        return
    fi
    echo "-- rknn: 下载运行库 $RKNN_VERSION"
    curl -fsSL -o "$so" "$RKNN_URL/aarch64/librknnrt.so"
    curl -fsSL -o "$hdr" "$RKNN_URL/include/rknn_api.h"
    if [[ "$(sha256_of "$so")" != "$RKNN_SO_SHA" ]]; then
        echo "错误: librknnrt.so SHA-256 与固定值不符" >&2
        exit 1
    fi
    if [[ "$(sha256_of "$hdr")" != "$RKNN_H_SHA" ]]; then
        echo "错误: rknn_api.h SHA-256 与固定值不符" >&2
        exit 1
    fi
}

configure_and_build() {
    # 包只出交付产物，单测开关全关（x86 CI 与 tests 预设负责单测）。
    local args=(
        -S "$REPO_ROOT" -B "$build/build" -G Ninja
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
        -DCMAKE_BUILD_TYPE=Release
        -DMPP_INSTALL_PREFIX="$deps/mpp"
        -DRKNN_INSTALL_PREFIX="$deps/rknn"
        -DRKVC_CORE_BUILD_TESTS=OFF
        -DRKVC_H264H265_BUILD_TESTS=OFF
        -DRKVC_AV1_BUILD_TESTS=OFF
        -DRKVC_MLVC_BUILD_TESTS=OFF
        -DRKVC_SR_BUILD_TESTS=OFF
        -DRKVC_PSPACK_BUILD_TESTS=OFF
        -DRKVC_CLI_BUILD_TESTS=OFF
    )
    if ((with_av1)); then
        args+=(-DSVT_AV1_INSTALL_PREFIX="$deps/svt")
    else
        args+=(-DRKVC_AV1_WITH_SVT=OFF)
    fi
    if ((!with_rknn)); then
        args+=(-DRKVC_MLVC_WITH_RKNN=OFF -DRKVC_SR_WITH_RKNN=OFF)
    fi
    run_logged "$logs/configure.log" "rkvc 配置" cmake "${args[@]}"
    run_logged "$logs/build.log" "rkvc 构建" cmake --build "$build/build" \
        -j "$jobs"
}

collect_artifacts() {
    # 依赖缺失时 codec 工程只 warn-and-skip，这里把静默跳过变成硬失败。
    artifacts=("$build/build/cli/rkvc")
    local expect=(rkvc_h264h265.so rkvc_mlvc.so rkvc_sr.so)
    if ((with_av1)); then
        expect+=(rkvc_av1.so)
    fi
    local name dir so
    for name in "${expect[@]}"; do
        dir="${name#rkvc_}"
        dir="${dir%.so}"
        so="$build/build/codecs/$dir/$name"
        if [[ ! -f "$so" ]]; then
            echo "错误: 预期插件缺失: $so（见 $logs/configure.log）" >&2
            exit 1
        fi
        artifacts+=("$so")
    done
    plugin_sos=("${artifacts[@]:1}")
}

bundle_runtime_libs() {
    # 只收 NEODED 里出现、且不在系统清单里的运行库；来源限定已构建前缀，
    # 防止把宿主 x86 库混进包。
    local dep src dev
    for dep in $(
        for f in "${artifacts[@]}"; do readelf -d "$f"; done |
            sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p' | sort -u
    ); do
        if is_system_lib "$dep"; then
            continue
        fi
        src="$(find "$deps/mpp/lib" "$deps/svt/lib" "$deps/rknn/lib" \
            -maxdepth 1 -name "$dep" -print -quit 2>/dev/null || true)"
        if [[ -z "$src" ]]; then
            echo "错误: 依赖 $dep 不在已构建前缀中，拒绝从宿主取库" >&2
            exit 1
        fi
        cp -Lf "$src" "$pkg/lib/$dep"
        chmod 644 "$pkg/lib/$dep"
        dev="${dep%%.so.*}.so"
        if [[ "$dev" != "$dep" ]]; then
            ln -sfn "$dep" "$pkg/lib/$dev"
        fi
    done
}

collect_licenses() {
    local lic="$pkg/licenses" f
    cp -f "$REPO_ROOT/LICENSE" "$lic/rkvc-AGPL-3.0-or-later.txt"
    for f in "$REPO_ROOT"/third_party/mpp/LICENSES/*; do
        [[ -f "$f" ]] && cp -f "$f" "$lic/mpp-$(basename "$f").txt"
    done
    if ((with_av1)); then
        for f in LICENSE.md LICENSE-BSD2.md PATENTS.md; do
            [[ -f "$REPO_ROOT/third_party/SVT-AV1/$f" ]] &&
                cp -f "$REPO_ROOT/third_party/SVT-AV1/$f" "$lic/svt-av1-$f"
        done
    fi
    {
        echo "rkvc $version linux-aarch64 portable package"
        echo "built: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "toolchain: aarch64-linux-gnu (Ubuntu 22.04, tools/portable/Dockerfile)"
        echo "third_party/mpp: $(submodule_commit mpp)"
        if ((with_av1)); then
            echo "third_party/SVT-AV1: $(submodule_commit SVT-AV1)"
        fi
        if ((with_rknn)); then
            echo "librknnrt.so: $RKNN_VERSION sha256=$RKNN_SO_SHA"
            echo "  source: $RKNN_URL/aarch64/librknnrt.so"
        fi
    } >"$lic/PROVENANCE.txt"
}

pack_models() {
    local count=0 f
    for f in "$models_dir"/*.rkmodel; do
        [[ -f "$f" ]] || continue
        install -m 644 "$f" "$pkg/models/"
        count=$((count + 1))
    done
    if ((count == 0)); then
        echo "错误: $models_dir 下没有 .rkmodel" >&2
        exit 1
    fi
    echo "-- models: 收录 $count 个 .rkmodel"
}

render_files() {
    local date
    date="$(date -u +%Y-%m-%d)"
    sed -e "s/@VERSION@/$version/g" -e "s/@DATE@/$date/g" \
        "$SCRIPT_DIR/package/test.sh" >"$pkg/test.sh"
    chmod 755 "$pkg/test.sh"
    sed -e "s/@VERSION@/$version/g" -e "s/@DATE@/$date/g" \
        "$SCRIPT_DIR/package/README.md" >"$pkg/README.md"
}

write_manifest() {
    (
        cd "$stage"
        find "$pkg_name" -type f ! -name MANIFEST.sha256 | LC_ALL=C sort |
            xargs sha256sum >"$pkg/MANIFEST.sha256"
    )
}

assemble_package() {
    rm -rf "$pkg"
    mkdir -p "$pkg/bin" "$pkg/lib/rkvc/backends" "$pkg/licenses" "$pkg/models"
    install -m 755 "$build/build/cli/rkvc" "$pkg/bin/rkvc"
    local so
    for so in "${plugin_sos[@]}"; do
        install -m 755 "$so" "$pkg/lib/rkvc/backends/"
    done
    bundle_runtime_libs
    collect_licenses
    if [[ -n "$models_dir" ]]; then
        pack_models
    else
        rmdir "$pkg/models"
    fi
    render_files
    write_manifest
}

verify_package() {
    echo "-- 审计: RUNPATH、依赖闭包、符号"
    local f entry rp provided dep
    for f in "$pkg/bin/rkvc" "$pkg"/lib/rkvc/backends/*.so "$pkg"/lib/*.so*; do
        [[ -f "$f" ]] || continue
        rp="$(readelf -d "$f" | sed -n 's/.*RUNPATH.*\[\(.*\)\].*/\1/p')"
        [[ -n "$rp" ]] || continue
        IFS=':' read -r -a entries <<<"$rp"
        for entry in "${entries[@]}"; do
            case "$entry" in
                '$ORIGIN'*) ;;
                *)
                    echo "错误: $(basename "$f") RUNPATH 含非相对项: $entry" >&2
                    exit 1
                    ;;
            esac
        done
    done
    provided="$(for f in "$pkg"/lib/*.so*; do basename "$f"; done)"
    for f in "$pkg/bin/rkvc" "$pkg"/lib/rkvc/backends/*.so "$pkg"/lib/*.so*; do
        [[ -f "$f" ]] || continue
        for dep in $(readelf -d "$f" |
            sed -n 's/.*NEEDED.*\[\(.*\)\].*/\1/p'); do
            if is_system_lib "$dep"; then
                continue
            fi
            if grep -qxF "$dep" <<<"$provided"; then
                continue
            fi
            echo "错误: $(basename "$f") 依赖 $dep 未随包提供" >&2
            exit 1
        done
    done
    run_logged "$logs/check-symbols.log" "符号审计" \
        bash "$REPO_ROOT/tools/check-symbols.sh" \
        "$pkg/bin/rkvc" "$pkg"/lib/rkvc/backends/*.so
}

run_selftest() {
    if command -v qemu-aarch64-static >/dev/null 2>&1; then
        echo "-- 包内自测（qemu-aarch64-static 模拟运行）"
        if ! RKVC_RUNNER="qemu-aarch64-static -L /usr/aarch64-linux-gnu" \
            bash "$pkg/test.sh"; then
            echo "错误: 包内自测失败" >&2
            exit 1
        fi
    else
        echo "-- 跳过包内自测: 无 qemu-aarch64-static，请在目标板跑 test.sh"
    fi
}

make_tarball() {
    local tarball="$out_dir/$pkg_name.tar.gz"
    tar --sort=name --owner=0 --group=0 --numeric-owner \
        -czf "$tarball" -C "$stage" "$pkg_name"
    (cd "$out_dir" && sha256sum "$pkg_name.tar.gz" >"$pkg_name.tar.gz.sha256")
    echo
    echo "== 完成 =="
    ls -l "$tarball" "$tarball.sha256"
}

echo "== rkvc $version 可移植包（linux-aarch64）=="
if ((clean)); then
    echo "-- --clean: 已清空 $build"
fi
build_mpp
if ((with_av1)); then
    build_svt
fi
if ((with_rknn)); then
    prepare_rknn
fi
configure_and_build
collect_artifacts
assemble_package
verify_package
run_selftest
make_tarball
