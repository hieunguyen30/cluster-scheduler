#!/usr/bin/env bash
# Usage: scripts/summarize.sh <mode>  -> build + test in <mode>, print a compact verdict.
cd "$(dirname "$0")/.."
mode="${1:-plain}"
log="/tmp/kestrel_$mode.log"
bash scripts/build.sh "$mode" >"$log" 2>&1
echo "exit=$?"
grep -E 'error:|WARNING: ThreadSanitizer|ERROR: AddressSanitizer|runtime error|\*\*\*Failed|tests passed|tests failed' "$log" | sort | uniq -c | head -20
