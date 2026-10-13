#!/usr/bin/env bash
# Print the first N TSan reports (full stacks, trimmed to Kestrel frames) from a test run.
# Usage: scripts/tsan_detail.sh <binary> <gtest_filter> [num_reports]
cd "$(dirname "$0")/.."
bin="${1:?binary}"; filter="${2:?filter}"; n="${3:-2}"
export KESTREL_TIME_SCALE=3
export TSAN_OPTIONS="halt_on_error=0 ignore_noninstrumented_modules=1 second_deadlock_stack=1 suppressions=$PWD/tests/tsan.supp"
out=$(mktemp)
setarch "$(uname -m)" -R "$HOME/kestrel-build/tsan/tests/$bin" --gtest_filter="$filter" >"$out" 2>&1
awk -v n="$n" '/WARNING: ThreadSanitizer/{c++} c>=1 && c<=n {print}' "$out" \
  | grep -vE 'gtest|testing::|HandleSehExceptions|HandleExceptions|RUN_ALL|gtest_main' \
  | sed -E 's|/mnt/c/Users/09041/Documents/PersonalProject/cluster-scheduler/cluster-scheduler/||; s| \(BuildId:[^)]*\)||' \
  | cut -c1-220 | head -120
rm -f "$out"
