#!/usr/bin/env bash
# 批量生成 AV1 + 3×AI 上采样左右对比演示视频
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:-$ROOT/bench/demo_videos.json}"
PY="$ROOT/bench/tools/comparison_demo_rkvc.py"

mkdir -p "$ROOT/bench/results/demos"

exec python3 "$PY" --config "$CONFIG" "${@:2}"
