#!/usr/bin/env bash
# Run one test (filter) from one build mode with the sanitizer environment, showing output.
# Usage: scripts/run_one.sh <mode> <binary> <gtest_filter> [max_lines]
cd "$(dirname "$0")/.."
mode="${1:?mode}"; bin="${2:?binary}"; filter="${3:?filter}"; lines="${4:-60}"
case "$mode" in tsan) export KESTREL_TIME_SCALE=3 ;; asan) export KESTREL_TIME_SCALE=2 ;; esac
export TSAN_OPTIONS="halt_on_error=0 ignore_noninstrumented_modules=1 suppressions=$PWD/tests/tsan.supp"
runner=""
[ "$mode" = tsan ] && runner="setarch $(uname -m) -R"
$runner "$HOME/kestrel-build/$mode/tests/$bin" --gtest_filter="$filter" --gtest_repeat="${REPEAT:-1}" 2>&1 | head -n "$lines"
