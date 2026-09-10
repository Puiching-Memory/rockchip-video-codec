#!/bin/bash
# 格式校验：仓库内 C/C++ 源码必须与根目录 .clang-format 一致。
# 用法: tools/check-format.sh [--fix]
# 退出码: 0 通过；1 存在未格式化文件（--fix 就地修复）；2 工具缺失。
# 结论依赖 clang-format 大版本（CI 固定 18），本地请用同一大版本复现。
set -euo pipefail

cd "$(dirname "$0")/.."

cf=${CLANG_FORMAT:-clang-format-18}
command -v "$cf" >/dev/null 2>&1 || cf=clang-format
command -v "$cf" >/dev/null 2>&1 || {
    echo "check-format: clang-format not found" >&2
    exit 2
}

# Vendored trees opt out via their own DisableFormat config, so no path
# filtering is needed here.
mapfile -t files < <(git ls-files '*.c' '*.cc' '*.cpp' '*.h' '*.hpp')
if [ "${1:-}" = "--fix" ]; then
    "$cf" -i --style=file "${files[@]}"
    exit 0
fi
"$cf" --dry-run --Werror --style=file "${files[@]}"
