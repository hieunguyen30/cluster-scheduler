#!/usr/bin/env bash
# Usage: scripts/build.sh [plain|asan|tsan]
# Builds out-of-tree into $KESTREL_BUILD_ROOT/<mode> (default ~/kestrel-build/<mode>, on the
# fast WSL filesystem even when the source lives on /mnt/c) and runs ctest.
set -euo pipefail
cd "$(dirname "$0")/.."
mode="${1:-plain}"
case "$mode" in
  plain) san="" ;;
  asan)  san="address" ;;
  tsan)  san="thread" ;;
  *) echo "unknown mode $mode" >&2; exit 2 ;;
esac
dir="${KESTREL_BUILD_ROOT:-$HOME/kestrel-build}/$mode"
cmake -S . -B "$dir" -G Ninja -DKESTREL_SANITIZE="$san" >/dev/null
cmake --build "$dir" -j"$(nproc)"
if [ "$mode" = tsan ]; then export TSAN_OPTIONS="halt_on_error=1 second_deadlock_stack=1"; fi
if [ "$mode" = asan ]; then export ASAN_OPTIONS="detect_leaks=1:abort_on_error=0"; fi
ctest --test-dir "$dir" --output-on-failure -j"$(nproc)"
