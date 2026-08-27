#!/bin/bash
# Verify that librkvc exports only API functions declared by public headers.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
LIB="${1:-}"

if [[ -z "$LIB" || ! -f "$LIB" ]]; then
    echo "用法: $0 /path/to/librkvc.so" >&2
    exit 2
fi

tmp_dir="$(mktemp -d)"
trap 'rm -rf "$tmp_dir"' EXIT

rg -o --no-filename 'rkvc_[A-Za-z0-9_]+(?=[[:space:]]*\()' \
    --pcre2 "$PROJECT_DIR/include/rkvc"/*.h | sort -u >"$tmp_dir/allowed"
rg -o --no-filename '^[[:space:]]*rkvc_[A-Za-z0-9_]+' \
    "$PROJECT_DIR/librkvc.map" | sed 's/^[[:space:]]*//' | sort -u \
    >"$tmp_dir/versioned"
nm -D --defined-only "$LIB" | awk '{print $3}' | sed 's/@.*//' | \
    grep '^rkvc_' | sort -u >"$tmp_dir/exported"

if ! cmp -s "$tmp_dir/allowed" "$tmp_dir/versioned"; then
    echo "错误: librkvc.map 与 public headers 不一致:" >&2
    diff -u "$tmp_dir/allowed" "$tmp_dir/versioned" >&2 || true
    exit 1
fi

if comm -23 "$tmp_dir/exported" "$tmp_dir/allowed" >"$tmp_dir/unexpected" && \
   [[ -s "$tmp_dir/unexpected" ]]; then
    echo "错误: librkvc 导出了未声明的内部符号:" >&2
    sed 's/^/  /' "$tmp_dir/unexpected" >&2
    exit 1
fi

echo "OK: librkvc 动态符号均来自 public headers ($(wc -l <"$tmp_dir/exported") symbols)"
