#!/usr/bin/env bash
# Repeat a test binary under a sanitizer to look for flakes and races.
# Usage: scripts/stress.sh <plain|asan|tsan> <test-binary-name> [repeat]
set -uo pipefail
mode="${1:?mode}"; bin="${2:?binary}"; n="${3:-5}"
root="${KESTREL_BUILD_ROOT:-$HOME/kestrel-build}/$mode"
export TSAN_OPTIONS="halt_on_error=0 ignore_noninstrumented_modules=1 second_deadlock_stack=1 suppressions=$(cd "$(dirname "$0")/.." && pwd)/tests/tsan.supp"
export ASAN_OPTIONS="detect_leaks=1"
case "$mode" in tsan) export KESTREL_TIME_SCALE=3 ;; asan) export KESTREL_TIME_SCALE=2 ;; esac
err="$(mktemp)"
runner=""
[ "$mode" = tsan ] && runner="setarch $(uname -m) -R"  # TSan vs high-entropy ASLR (see CMakeLists)
$runner "$root/tests/$bin" --gtest_repeat="$n" 2>"$err" | grep -E '^\[ +(FAILED|PASSED) +\]|measured' | sort | uniq -c
echo "sanitizer reports: $(grep -cE 'WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error' "$err")"
rm -f "$err"
