#!/usr/bin/env bash
# Real-process control-plane crash test: SIGKILL the control plane mid-run, restart it from its
# WAL, and verify no job is lost or run twice. Usage: scripts/demo_restart.sh [build-dir]
set -euo pipefail
B="${1:-$HOME/kestrel-build/plain}/src"
PORT="${PORT:-17100}"
WORK="$(mktemp -d)"
RUNS="$WORK/runs.txt"
cleanup() { kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT
CTL="$B/kestrelctl --control 127.0.0.1:$PORT"

start_control() {
  "$B/kestrel_control" --listen 127.0.0.1:$PORT --wal-path "$WORK/kestrel.wal" \
      --suspect-after-ms 1500 --dead-after-ms 3000 2>>"$WORK/control.log" &
  CP=$!
  sleep 0.7
}

start_control
for n in a b c; do
  "$B/kestrel_worker" --control 127.0.0.1:$PORT --name worker-$n --cpu-millis 2000 \
      --heartbeat-ms 200 2>"$WORK/$n.log" &
done
sleep 1

echo "== submitting 12 jobs (each ~2s, appends its index to a file)"
for i in $(seq 1 12); do
  $CTL submit --cpu 700 --mem 64 --request-id "demo-$i" -- sh -c "echo $i >> $RUNS; sleep 2" >/dev/null
done
sleep 1.5
echo "== SIGKILL control plane (pid $CP) with jobs running"
kill -9 $CP; wait $CP 2>/dev/null || true
sleep 1
echo "== restarting control plane from the WAL"
start_control
grep -E 'wal|recovered' "$WORK/control.log" | tail -n 3
$CTL list | awk '{print $3}' | sort | uniq -c
echo "== resubmitting job 3 with the same request id (client retry): expect the same job id"
$CTL submit --cpu 700 --mem 64 --request-id "demo-3" -- sh -c "echo 3 >> $RUNS; sleep 2"

echo "== waiting for all jobs"
for i in $(seq 1 12); do $CTL wait "$i" --timeout-s 60 >/dev/null; done
$CTL list | awk '{print $3}' | sort | uniq -c
echo "== executions per job (expect each exactly once):"
sort -n "$RUNS" | uniq -c | awk '{print $1}' | sort | uniq -c | sed 's/^/   (count of jobs, runs) /'
echo "total job lines: $(wc -l < "$RUNS") for 12 jobs"
