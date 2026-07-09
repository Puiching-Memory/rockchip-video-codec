#!/bin/bash
# scripts/check-version-literals.sh — 防止文档/CI 写死当前版本号
#
# 检查范围（避免误伤 CHANGELOG / 里程碑历史）:
#   README.md、docs/packaging.md、docs/release/README.md、.github/workflows/ci.yml
# 禁止出现:
#   - rkvc-<当前版本>-
#   - rkvc_<当前版本>_
#   - **版本**: <当前版本>
#
# 用法: ./scripts/check-version-literals.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"

ROOT="$(rkvc_repo_root)"
VERSION="$(rkvc_project_version)"

FILES=(
    "$ROOT/README.md"
    "$ROOT/docs/packaging.md"
    "$ROOT/docs/release/README.md"
    "$ROOT/.github/workflows/ci.yml"
)

patterns=(
    "rkvc-${VERSION}-"
    "rkvc_${VERSION}_"
    "**版本**: ${VERSION}"
    "**版本**:${VERSION}"
)

failed=0
for f in "${FILES[@]}"; do
    [[ -f "$f" ]] || continue
    for pat in "${patterns[@]}"; do
        if grep -F -n -- "$pat" "$f" >/dev/null 2>&1; then
            echo "错误: $f 含当前版本硬编码: $pat"
            grep -F -n -- "$pat" "$f" | sed 's/^/  /'
            failed=1
        fi
    done
done

if [[ $failed -ne 0 ]]; then
    echo "请改为通配符 / rkvc_portable_pkg_dir / rkvc_info -v；版本唯一来源为 CMakeLists.txt project(VERSION)。"
    exit 1
fi

echo "OK: 无当前版本硬编码 (version=$VERSION)"
