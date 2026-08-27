#!/bin/bash
# scripts/build-models.sh — 为指定目标平台生产 RKNN 模型 bundle（暂存，不入库）
#
# 用法:
#   ./scripts/build-models.sh --platform rk3576
#   ./scripts/build-models.sh --platform rk3588 --sr-weight /path/to/best_ema.pth
#   ./scripts/build-models.sh --platform rv1126b --allow-skip-sr --clean
#
# 选项:
#   --platform <soc>     目标平台（必填）：rk3588 / rk3576 / rk3568 / rk3566 / rv1126b
#   --variants LIST      逗号分隔的 MLVC 变体：mlvc / mlvc-s（默认 mlvc-s）
#   --sr-weight <path>   本地 SR 权重（best_ema.pth）；缺省自动从 HuggingFace 下载
#   --allow-skip-sr      SR 权重不可得/导出失败时警告跳过（默认报错）
#   --clean              清理该平台暂存产物后重建
#
# 产物（暂存目录 .build/models/<platform>/，不污染仓库 models/）:
#   mlvc/    mlvc-s/    rkvc-sr/
#
# 权重来源（均可自动下载，详见 docs/packaging.md）:
#   MLVC     mlvideopub.blob.core.windows.net/mlvc/models/mlvc-psnr-v1.ckpt   (SHA-256 钉住)
#   MLVC-S   mlvideopub.blob.core.windows.net/mlvc/models/mlvc-s-psnr-v1.ckpt (SHA-256 钉住)
#   SR       huggingface.co/Sail2Dream/phase-rlfn-codec-v1 (best_ema.pth, QAT 免校准)
#
# 幂等：产物齐全且校验通过则跳过；--clean 强制重建。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

PY="$PROJECT_DIR/.venv/bin/python"
STAGING_ROOT="$PROJECT_DIR/.build/models"
ONNX_CACHE="$STAGING_ROOT/onnx-cache"
SR_WEIGHT_DIR="$PROJECT_DIR/.build/deps/sr-weights/phase-rlfn-codec-v1"

SR_WEIGHT_URL="${RKVC_SR_WEIGHT_URL:-https://huggingface.co/Sail2Dream/phase-rlfn-codec-v1/resolve/main/best_ema.pth}"
SR_WEIGHT_SHA256="${RKVC_SR_WEIGHT_SHA256:-0cf78ceeb6f629fee11dfc6ad2237dece0c2e7e721c553a57b84bd4ee2c84070}"

MLVC_QP=21
MLVC_QP_LIST="0,21,42,63"
VALID_PLATFORMS="rk3588 rk3576 rk3568 rk3566 rv1126b"

PLATFORM=""
VARIANTS="mlvc-s"
SR_WEIGHT=""
ALLOW_SKIP_SR=0
CLEAN=0

usage() {
    sed -n '2,25p' "$0"
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --platform)       PLATFORM="$2"; shift 2 ;;
        --variants)       VARIANTS="$2"; shift 2 ;;
        --sr-weight)      SR_WEIGHT="$2"; shift 2 ;;
        --allow-skip-sr)  ALLOW_SKIP_SR=1; shift ;;
        --clean)          CLEAN=1; shift ;;
        -h|--help)        usage; exit 0 ;;
        *) echo "错误: 未知参数 '$1'" >&2; usage; exit 2 ;;
    esac
done

if [[ -z "$PLATFORM" ]]; then
    echo "错误: 必须指定 --platform" >&2
    exit 2
fi
case " $VALID_PLATFORMS " in
    *" $PLATFORM "*) ;;
    *)
        echo "错误: 不支持的平台 '$PLATFORM'（可选: $VALID_PLATFORMS）" >&2
        exit 2
        ;;
esac

# 解析并校验变体列表（默认仅 mlvc-s）
VARIANT_LIST=()
IFS=',' read -r -a _variant_parts <<< "$VARIANTS"
for v in "${_variant_parts[@]}"; do
    v="${v// /}"
    [[ "$v" == all ]] && { VARIANT_LIST=(mlvc mlvc-s); break; }
    if [[ "$v" != mlvc && "$v" != mlvc-s ]]; then
        echo "错误: 不支持的变体 '$v'（可选: mlvc / mlvc-s / all）" >&2
        exit 2
    fi
    VARIANT_LIST+=("$v")
done
[[ ${#VARIANT_LIST[@]} -gt 0 ]] || { echo "错误: --variants 为空" >&2; exit 2; }

if [[ ! -x "$PY" ]]; then
    echo "错误: 缺少 $PY，请先在仓库根目录运行: uv sync" >&2
    exit 1
fi

STAGING="$STAGING_ROOT/$PLATFORM"
MLVC_DIR="$STAGING/mlvc"
MLVC_S_DIR="$STAGING/mlvc-s"
SR_DIR="$STAGING/rkvc-sr"

if [[ $CLEAN -eq 1 ]]; then
    rm -rf "$STAGING"
fi
mkdir -p "$STAGING"

sha256_of() {
    sha256sum "$1" | awk '{print $1}'
}

# HF LFS 重定向对 curl 偶尔报 SSL unexpected EOF；用项目 venv 的 urllib 下载
# （与 tools/mlvc/export_onnx.py 的 download_file 一致，自动跟随 302 到 CDN）。
download_to() {
    local url="$1" dest="$2"
    "$PY" - "$url" "$dest" <<'PYEOF'
import shutil, sys, urllib.request

url, dest = sys.argv[1], sys.argv[2]
tmp = dest + ".part"
with urllib.request.urlopen(url, timeout=60) as resp, open(tmp, "wb") as fp:
    shutil.copyfileobj(resp, fp)
import os
os.replace(tmp, dest)
PYEOF
}

# ── SR 权重准备（跨平台共享） ──────────────────────────────────────────
prepare_sr_weight() {
    if [[ -n "$SR_WEIGHT" ]]; then
        if [[ ! -f "$SR_WEIGHT" ]]; then
            echo "错误: --sr-weight 不存在: $SR_WEIGHT" >&2
            return 1
        fi
        echo "  SR 权重 (本地): $SR_WEIGHT"
        return 0
    fi
    local dest="$SR_WEIGHT_DIR/best_ema.pth"
    if [[ -f "$dest" ]] && [[ "$(sha256_of "$dest")" == "$SR_WEIGHT_SHA256" ]]; then
        echo "  SR 权重已就绪: $dest"
        SR_WEIGHT="$dest"
        return 0
    fi
    mkdir -p "$SR_WEIGHT_DIR"
    echo "  GET $SR_WEIGHT_URL"
    local tmp="${dest}.part"
    if ! download_to "$SR_WEIGHT_URL" "$dest"; then
        rm -f "$tmp"
        echo "错误: SR 权重下载失败: $SR_WEIGHT_URL" >&2
        echo "  可用 --sr-weight 指定本地 best_ema.pth（HuggingFace: Sail2Dream/phase-rlfn-codec-v1）" >&2
        return 1
    fi
    local digest
    digest="$(sha256_of "$dest")"
    if [[ "$digest" != "$SR_WEIGHT_SHA256" ]]; then
        rm -f "$dest"
        echo "错误: SR 权重 SHA-256 不符: $digest" >&2
        return 1
    fi
    SR_WEIGHT="$dest"
    return 0
}

# ── MLVC 变体：ONNX 导出（平台无关，缓存）+ RKNN 转换（按平台） ──────
# $1=变体(mlvc|mlvc-s)  $2=额外 model-version 参数(空=标准版)
mlvc_variant_dir() {
    [[ "$1" == "mlvc-s" ]] && echo "$MLVC_S_DIR" || echo "$MLVC_DIR"
}

mlvc_onnx_ready() {
    local variant="$1"
    [[ -f "$ONNX_CACHE/$variant.stamp" ]] || return 1
    local onnx_dir
    onnx_dir="$(cat "$ONNX_CACHE/$variant.stamp")"
    [[ -f "$onnx_dir/MLVCEncoder.onnx" && -f "$onnx_dir/MLVCDecoder.onnx" ]]
}

mlvc_export_onnx() {
    local variant="$1" out_dir="$2" mv_arg=()
    [[ "$variant" == "mlvc-s" ]] && mv_arg=(--model-version dmc61sbr_reglu_s)
    mkdir -p "$ONNX_CACHE" "$out_dir"
    local log="$ONNX_CACHE/$variant.onnx.log"
    echo "  导出 $variant ONNX（--from-mlvc --pmf-only，含权重自动下载）…"
    if ! "$PY" "$PROJECT_DIR/tools/mlvc/export_rknn.py" \
            --from-mlvc --pmf-only \
            "${mv_arg[@]+"${mv_arg[@]}"}" \
            --out-dir "$out_dir" >"$log" 2>&1; then
        tail -20 "$log" >&2
        echo "错误: $variant ONNX 导出失败（日志: $log）" >&2
        return 1
    fi
    local onnx_dir
    onnx_dir="$(sed -n 's/^ONNX 目录: //p' "$log" | tail -1)"
    if [[ -z "$onnx_dir" || ! -f "$onnx_dir/MLVCEncoder.onnx" ]]; then
        tail -20 "$log" >&2
        echo "错误: 未能从导出日志解析 ONNX 目录" >&2
        return 1
    fi
    printf '%s\n' "$onnx_dir" > "$ONNX_CACHE/$variant.stamp"
    echo "  $variant ONNX 目录: $onnx_dir"
}

mlvc_bundle_ok() {
    local out_dir="$1" plat="$2"
    [[ -f "$out_dir/MLVCEncoder_${plat}.rknn" && \
       -f "$out_dir/MLVCDecoder_${plat}.rknn" && \
       -f "$out_dir/gaussian.bin" && \
       -f "$out_dir/bitest.bin" && \
       -f "$out_dir/mlvc_rknn_export_manifest.json" && \
       -f "$out_dir/qp_patches/${plat}/enc_qp${MLVC_QP}.qppatch" ]]
}

# 逐 QP 全量模型（export_rknn.py --qp-list 写入 <out_dir>/<platform>_qp_models/）只是
# 推导 qp_patches 的中间件（单变体约 311MB），留在暂存 bundle 里会被
# package-portable.sh 整目录 cp 进包（实测包体 92M→403M、多出 8 个 .rknn）。
# manifest 记录的是打包机绝对路径（provenance，与 ONNX 源路径同一语义），
# 删除这些中间件不影响包内校验。
mlvc_prune_qp_models() {
    local out_dir="$1" tmp
    for tmp in "$out_dir"/*_qp_models; do
        [[ -d "$tmp" ]] && rm -rf "$tmp"
    done
    return 0
}

mlvc_build_variant() {
    local variant="$1" mv_arg=()
    local out_dir
    out_dir="$(mlvc_variant_dir "$variant")"
    [[ "$variant" == "mlvc-s" ]] && mv_arg=(--model-version dmc61sbr_reglu_s)

    # 旧版缓存可能残留中间件，先清再判定是否已就绪
    mlvc_prune_qp_models "$out_dir"
    if mlvc_bundle_ok "$out_dir" "$PLATFORM"; then
        echo "--- $variant: 暂存产物已就绪，跳过 ($out_dir) ---"
        return 0
    fi

    if ! mlvc_onnx_ready "$variant"; then
        mlvc_export_onnx "$variant" "$out_dir"
    fi
    local onnx_dir
    onnx_dir="$(cat "$ONNX_CACHE/$variant.stamp")"

    echo "--- $variant: RKNN 转换 (platform=$PLATFORM qp_list=$MLVC_QP_LIST) ---"
    mkdir -p "$out_dir"
    "$PY" "$PROJECT_DIR/tools/mlvc/export_rknn.py" \
        --onnx-dir "$onnx_dir" \
        "${mv_arg[@]+"${mv_arg[@]}"}" \
        --out-dir "$out_dir" \
        --platform "$PLATFORM" \
        --qp "$MLVC_QP" --qp-list "$MLVC_QP_LIST" \
        --patch-dir "$out_dir/qp_patches/$PLATFORM"
    mlvc_prune_qp_models "$out_dir"
    if ! mlvc_bundle_ok "$out_dir" "$PLATFORM"; then
        echo "错误: $variant bundle 不完整: $out_dir" >&2
        return 1
    fi
    echo "  $variant bundle 完成: $out_dir"
}

# ── SR bundle（按平台） ────────────────────────────────────────────────
sr_bundle_ok() {
    "$PY" "$PROJECT_DIR/tools/sr/verify_bundle.py" "$SR_DIR" >/dev/null 2>&1
}

sr_build() {
    if sr_bundle_ok; then
        echo "--- rkvc-sr: 暂存产物已就绪，跳过 ($SR_DIR) ---"
        return 0
    fi
    if ! prepare_sr_weight; then
        if [[ $ALLOW_SKIP_SR -eq 1 ]]; then
            echo "警告: SR 权重不可得，跳过 rkvc-sr bundle（--allow-skip-sr）"
            return 0
        fi
        return 1
    fi
    echo "--- rkvc-sr: 导出 (target=$PLATFORM, QAT 免校准 --no-quantize) ---"
    mkdir -p "$SR_DIR"
    if ! "$PY" "$PROJECT_DIR/tools/sr/export_model.py" \
            --weight "$SR_WEIGHT" \
            --no-quantize \
            --target "$PLATFORM" \
            --output-dir "$SR_DIR"; then
        if [[ $ALLOW_SKIP_SR -eq 1 ]]; then
            echo "警告: rkvc-sr 导出失败，跳过（--allow-skip-sr）"
            return 0
        fi
        echo "错误: rkvc-sr 导出失败" >&2
        return 1
    fi
    if ! sr_bundle_ok; then
        if [[ $ALLOW_SKIP_SR -eq 1 ]]; then
            echo "警告: rkvc-sr bundle 校验失败，跳过（--allow-skip-sr）"
            return 0
        fi
        echo "错误: rkvc-sr bundle 校验失败: $SR_DIR" >&2
        return 1
    fi
    echo "  rkvc-sr bundle 完成: $SR_DIR"
}

echo "=== 模型生产 (platform=$PLATFORM variants=${VARIANT_LIST[*]} → $STAGING) ==="
for variant in "${VARIANT_LIST[@]}"; do
    mlvc_build_variant "$variant"
done
sr_build
echo "=== 模型生产完成: $STAGING ==="
