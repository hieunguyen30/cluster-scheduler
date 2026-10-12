#!/usr/bin/env bash
# Multi-process smoke run on one machine: 1 control plane, 2 worker processes, jobs via the CLI.
# Usage: scripts/demo_local.sh [build-dir]
set -euo pipefail
B="${1:-$HOME/kestrel-build/plain}/src"
PORT="${PORT:-17000}"
WORK="$(mktemp -d)"
cleanup() { kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

"$B/kestrel_control" --listen 127.0.0.1:$PORT --suspect-after-ms 1500 --dead-after-ms 2500 2>"$WORK/control.log" &
sleep 0.5
"$B/kestrel_worker" --control 127.0.0.1:$PORT --name worker-a --cpu-millis 2000 --heartbeat-ms 200 2>"$WORK/a.log" &
"$B/kestrel_worker" --control 127.0.0.1:$PORT --name worker-b --cpu-millis 2000 --heartbeat-ms 200 2>"$WORK/b.log" &
sleep 1
CTL="$B/kestrelctl --control 127.0.0.1:$PORT"
$CTL workers
for i in 1 2 3 4; do $CTL submit --cpu 800 --mem 128 -- sh -c "sleep 1; echo job $i"; done
$CTL submit --cpu 100 --mem 64 --wait -- sh -c "exit 0"
sleep 4
$CTL list
$CTL workers
echo "--- control log (tail) ---"
tail -n 12 "$WORK/control.log"
