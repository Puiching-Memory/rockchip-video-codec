#!/bin/bash
# 符号审计：GLIBC_2.34 上限 + 禁动态 libstdc++/libgcc_s + 导出面核对。
# 用法: tools/check-symbols.sh <elf> [<elf>...]
# 退出码: 0 全过；1 违规；2 用法错误。
set -euo pipefail

MAX_GLIBC_MAJOR=2
# Ceiling 2.34, not 2.17: pthread_once/key/dlopen and __libc_start_main
# bind 2.34 even in the old production librkvc.so.0.4.0 (board-verified).
# librknnrt's 2.17 stays the aspiration pending a 2.17 sysroot; 2.34 keeps
# every artifact runnable on the glibc-2.35 fleet with one version margin.
MAX_GLIBC_MINOR=34

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
mkdir -p "$PROJECT_DIR/.temp"

fail=0
for ELF in "$@"; do
    if [[ $# -eq 0 ]]; then
        echo "用法: $0 <elf> [<elf>...]" >&2
        exit 2
    fi
    if [[ ! -f "$ELF" ]]; then
        echo "错误: 文件不存在: $ELF" >&2
        exit 2
    fi

    # 1. NEEDED 审计：禁止动态 libstdc++/libgcc_s（要求 -static-libstdc++ -static-libgcc）。
    needed="$(readelf -d "$ELF" 2>/dev/null | grep -oP '\[.*\]' | tr -d '[]' || true)"
    if echo "$needed" | grep -Eq 'libstdc\+\+|libgcc_s'; then
        echo "错误: $ELF 动态依赖 libstdc++/libgcc_s（缺 -static-libstdc++ -static-libgcc）:" >&2
        echo "$needed" | grep -E 'libstdc\+\+|libgcc_s' | sed 's/^/  /' >&2
        fail=1
    fi

    # 2. GLIBC 符号版本上限（见顶部门限说明）。
    ver_refs="$(nm -D --with-symbol-versions "$ELF" 2>/dev/null | grep -oP 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu || true)"
    worst=""
    for v in $ver_refs; do
        num="${v#GLIBC_}"
        maj="${num%%.*}"
        min="${num##*.}"
        if (( maj > MAX_GLIBC_MAJOR )) || { (( maj == MAX_GLIBC_MAJOR )) && (( min > MAX_GLIBC_MINOR )); }; then
            worst="$v"
        fi
    done
    if [[ -n "$worst" ]]; then
        echo "错误: $ELF 引用 $worst，超出 GLIBC_2.17 上限" >&2
        fail=1
    fi

    # 静态 libstdc++ 后不应再有 GLIBCXX/CXXABI 动态版本引用。
    if nm -D --with-symbol-versions "$ELF" 2>/dev/null | grep -Eq 'GLIBCXX_|CXXABI_'; then
        echo "错误: $ELF 含 GLIBCXX/CXXABI 动态引用（libstdc++ 未静态链接）" >&2
        fail=1
    fi
done

# 3. 导出面核对：仅当新 C ABI 头存在时执行（旧 C 树时期跳过）。
NEW_ABI="$PROJECT_DIR/core/include/rkvc/rkvc.h"
if [[ -f "$NEW_ABI" && $# -gt 0 ]]; then
    tmp_dir="$(mktemp -d "$PROJECT_DIR/.temp/symbols.XXXXXX")"
    trap 'rm -rf "$tmp_dir"' EXIT
    { sed '/^[[:space:]]*\(\/\*\|\*\|\/\/\)/d' "$NEW_ABI" | \
        grep -ohP 'rkvc_[A-Za-z0-9_]+(?=[[:space:]]*\()';
        # The plugin handshake entry is ABI surface too (plugin.hpp), but
        # lives outside the app-facing rkvc.h.
        echo rkvc_plugin_query; } | sort -u >"$tmp_dir/allowed"
    for ELF in "$@"; do
        if [[ "$ELF" == *.so* ]]; then
            nm -D --defined-only "$ELF" | awk '{print $3}' | sed 's/@.*//' | \
                grep '^rkvc_' | sort -u >"$tmp_dir/exported" || true
            if comm -23 "$tmp_dir/exported" "$tmp_dir/allowed" >"$tmp_dir/unexpected" && \
               [[ -s "$tmp_dir/unexpected" ]]; then
                echo "错误: $ELF 导出了头文件未声明的符号:" >&2
                sed 's/^/  /' "$tmp_dir/unexpected" >&2
                fail=1
            fi
        fi
    done
fi

if (( fail == 0 )); then
    echo "OK: 符号审计通过 ($*)"
fi
exit "$fail"
