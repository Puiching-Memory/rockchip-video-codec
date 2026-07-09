#!/bin/bash
# scripts/run-bench.sh — RK3588 端到端 RD 基准入口
#
# 用法:
#   ./scripts/run-bench.sh /path/to/video.mp4
#   RUN_CODECS=h264,rkvc RKVC_BUILD=.build/release ./scripts/run-bench.sh clip.mp4
#   PLOT_ONLY=1 ./scripts/run-bench.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH_SH="$PROJECT_DIR/bench/run_rd_benchmark.sh"

if [[ ! -x "$BENCH_SH" ]]; then
    chmod +x "$BENCH_SH"
fi

export RKVC_BUILD="${RKVC_BUILD:-${RKVC_BUILD_DIR:-$PROJECT_DIR/.build/release}}"
exec "$BENCH_SH" "$@"
