#!/usr/bin/env bash
# 批量生成 AV1 + 3×AI 上采样左右对比演示视频
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:-$ROOT/tools/bench/demo_videos.json}"
PY="$ROOT/tools/bench/tools/comparison_demo_rkvc.py"
TOOLS_PY="$ROOT/tools/.venv/bin/python"

if [[ ! -x "$TOOLS_PY" ]]; then
    echo "[error] 共享 Python 环境不存在: $TOOLS_PY" >&2
    echo "请先运行: cd $ROOT/tools && uv sync" >&2
    exit 1
fi

mkdir -p "$ROOT/tools/bench/results/demos"

exec "$TOOLS_PY" "$PY" --config "$CONFIG" "${@:2}"
