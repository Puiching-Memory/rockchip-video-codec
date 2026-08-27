#!/bin/bash
# Prepare the x86 host-side Python environment used by ONNX/RKNN exporters.
# uv itself is bootstrapped into .build/host when it is not already available;
# project packages remain locked by uv.lock and are installed into .venv.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UV_VERSION="${RKVC_UV_VERSION:-0.12.2}"
PYTHON_VERSION="${RKVC_MODEL_PYTHON_VERSION:-3.12}"
UV_DEFAULT_INDEX="${RKVC_UV_DEFAULT_INDEX:-https://pypi.org/simple}"
UV_BIN="${RKVC_UV:-}"

if [[ -n "$UV_BIN" ]]; then
    command -v "$UV_BIN" >/dev/null 2>&1 || {
        echo "错误: RKVC_UV 指定的程序不存在: $UV_BIN" >&2
        exit 1
    }
elif command -v uv >/dev/null 2>&1; then
    UV_BIN="$(command -v uv)"
else
    bootstrap="$PROJECT_DIR/.build/host/uv-bootstrap"
    UV_BIN="$bootstrap/bin/uv"
    if [[ ! -x "$UV_BIN" ]]; then
        echo "=== 安装宿主环境管理器 uv==$UV_VERSION ($bootstrap) ==="
        if ! python3 -m venv "$bootstrap"; then
            echo "错误: 无法创建 uv bootstrap venv；请安装 python3-venv" >&2
            exit 1
        fi
        "$bootstrap/bin/python" -m pip install \
            --disable-pip-version-check --no-input \
            --index-url "$UV_DEFAULT_INDEX" "uv==$UV_VERSION"
    fi
fi

echo "=== 同步模型导出环境 (Python $PYTHON_VERSION, uv.lock) ==="
cd "$PROJECT_DIR"
# Explicitly select the index recorded in uv.lock. This prevents a machine-wide
# ~/.config/uv/uv.toml mirror from rewriting or invalidating the project lock.
"$UV_BIN" sync \
    --default-index "$UV_DEFAULT_INDEX" \
    --locked --python "$PYTHON_VERSION"

MODEL_PY="$PROJECT_DIR/.venv/bin/python"
[[ -x "$MODEL_PY" ]] || {
    echo "错误: uv sync 后未生成 $MODEL_PY" >&2
    exit 1
}

"$MODEL_PY" - <<'PY'
import importlib

required = ("numpy", "onnx", "onnxruntime", "rknn", "torch")
missing = []
for name in required:
    try:
        importlib.import_module(name)
    except Exception as exc:  # include broken native wheels, not just ImportError
        missing.append(f"{name}: {exc}")
if missing:
    raise SystemExit("模型导出环境不可用:\n  " + "\n  ".join(missing))
PY

echo "--- 模型导出环境已就绪: $MODEL_PY ---"
