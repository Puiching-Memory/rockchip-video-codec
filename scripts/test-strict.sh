#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=build-common.sh
source "$SCRIPT_DIR/build-common.sh"
rkvc_limit_build_jobs

COVERAGE_MIN_LINE=${RKVC_COVERAGE_MIN_LINE:-0}
COVERAGE_MIN_BRANCH=${RKVC_COVERAGE_MIN_BRANCH:-0}
VALGRIND_HARDWARE=${RKVC_VALGRIND_HARDWARE:-0}

run_matrix() {
    local preset="$1"
    local build_dir="$2"

    rm -rf "$ROOT_DIR/$build_dir"

    echo "==> configure: $preset"
    cmake --preset "$preset"

    echo "==> build: $preset"
    cmake --build --preset "$preset" -j"$BUILD_JOBS"

    echo "==> test: $preset"
    ctest --preset "$preset" -j1 --output-on-failure
}

cd "$ROOT_DIR"

run_matrix tests .build/tests
run_matrix asan .build/asan
run_matrix coverage .build/coverage

if command -v valgrind >/dev/null 2>&1; then
    echo "==> valgrind: .build/tests"
    mapfile -t TEST_BINS < <(find "$ROOT_DIR/.build/tests" -maxdepth 1 -type f -perm -111 -name 'test_*' | sort)
    for test_bin in "${TEST_BINS[@]}"; do
        test_name="$(basename "$test_bin")"
        if [ "$test_name" = "test_hardware" ] || [ "$test_name" = "test_scale" ]; then
            if [ "$VALGRIND_HARDWARE" != "1" ]; then
                echo "==> skipping valgrind: $test_name (RKMPP/RGA/SVT; set RKVC_VALGRIND_HARDWARE=1 to enable)"
                continue
            fi
        fi
        echo "==> valgrind: $test_name"
        VALGRIND_SUPP="$ROOT_DIR/scripts/mpp.supp"
        valgrind --quiet \
            --leak-check=full \
            --show-leak-kinds=all \
            --track-origins=yes \
            --error-exitcode=1 \
            ${VALGRIND_SUPP:+--suppressions="$VALGRIND_SUPP"} \
            "$test_bin"
    done
else
    echo "==> skipping valgrind (not installed)"
fi

if command -v gcovr >/dev/null 2>&1; then
    echo "==> coverage summary"
    COVERAGE_DIR="$ROOT_DIR/.build/coverage/coverage"
    mkdir -p "$COVERAGE_DIR"

    GCOVR_ARGS=(
        --root "$ROOT_DIR"
        "$ROOT_DIR/.build/coverage"
        --exclude "$ROOT_DIR/third_party/.*"
        --exclude "$ROOT_DIR/tests/.*"
        --print-summary
        --xml-pretty
        --xml "$COVERAGE_DIR/coverage.xml"
        --html
        --html-details
        --output "$COVERAGE_DIR/index.html"
    )

    if [ "$COVERAGE_MIN_LINE" -gt 0 ]; then
        GCOVR_ARGS+=(--fail-under-line "$COVERAGE_MIN_LINE")
    fi
    if [ "$COVERAGE_MIN_BRANCH" -gt 0 ]; then
        GCOVR_ARGS+=(--fail-under-branch "$COVERAGE_MIN_BRANCH")
    fi

    gcovr "${GCOVR_ARGS[@]}"
    echo "==> coverage report: $COVERAGE_DIR/index.html"
else
    echo "==> skipping gcovr (not installed)"
fi
