#!/usr/bin/env bash
# List the distinct TSan report headlines (type + first user-visible frames) from one test binary.
# Usage: scripts/tsan_reports.sh <test-binary> [gtest args...]
cd "$(dirname "$0")/.."
bin="${1:?binary}"; shift
export TSAN_OPTIONS="halt_on_error=0 ignore_noninstrumented_modules=1 suppressions=$PWD/tests/tsan.supp"
out=$(mktemp)
setarch "$(uname -m)" -R "$HOME/kestrel-build/tsan/tests/$bin" "$@" >"$out" 2>&1
echo "reports: $(grep -c 'WARNING: ThreadSanitizer' "$out")"
grep -A3 'WARNING: ThreadSanitizer' "$out" | grep -E 'WARNING|#0|#1' | sed -E 's/\(.*//' | sort | uniq -c | sort -rn | head -30
grep -E '^\[  (PASSED|FAILED)' "$out" | sort | uniq -c
rm -f "$out"
