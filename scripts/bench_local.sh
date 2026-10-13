#!/usr/bin/env bash
# One real-process benchmark run on this machine: control plane (WITH a WAL, fsync every append)
# + N worker processes + loadgen.
# Usage: scripts/bench_local.sh <policy> <seed> [jobs] [workers] [load] [time-scale] [heartbeat-ms]
# Prints the loadgen RESULT line and a few control-plane metrics. Workers advertise the same
# heterogeneous logical capacities as the simulator fleet (2c/8GiB, 4c/16GiB, 8c/32GiB cycling);
# the processes themselves are tiny.
set -euo pipefail
POLICY="${1:?policy}"; SEED="${2:?seed}"
JOBS="${3:-200}"; WORKERS="${4:-9}"; LOAD="${5:-0.85}"; SCALE="${6:-20}"; HB="${7:-100}"
B="${KESTREL_BIN:-$HOME/kestrel-build/plain/src}"
PORT="${PORT:-$((20000 + RANDOM % 20000))}"
MPORT=$((PORT + 1))
WORK="$(mktemp -d)"
cleanup() { kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

"$B/kestrel_control" --listen 127.0.0.1:$PORT --policy "$POLICY" --metrics-port $MPORT \
    --wal-path "$WORK/kestrel.wal" --wal-sync every \
    --suspect-after-ms 3000 --dead-after-ms 5000 --log-level warn 2>"$WORK/control.log" &
sleep 0.5
cpus=(2000 4000 8000); mems=(8192 16384 32768)
for i in $(seq 0 $((WORKERS - 1))); do
  "$B/kestrel_worker" --control 127.0.0.1:$PORT --name "w$i" --cpu-millis "${cpus[$((i % 3))]}" \
      --mem-mib "${mems[$((i % 3))]}" --heartbeat-ms "$HB" --log-level warn 2>"$WORK/w$i.log" &
done
for _ in $(seq 1 100); do
  n=$("$B/kestrelctl" --control 127.0.0.1:$PORT workers | grep -c ALIVE || true)
  [ "$n" -ge "$WORKERS" ] && break
  sleep 0.1
done
"$B/kestrel_loadgen" --control 127.0.0.1:$PORT --seed "$SEED" --jobs "$JOBS" --workers "$WORKERS" \
    --load "$LOAD" --time-scale "$SCALE" --label "$POLICY" ${CSV:+--csv "$CSV"} | grep -E '^(RESULT|#)'
curl -s "http://127.0.0.1:$MPORT/metrics" | awk '
  /^kestrel_wal_append_latency_seconds_(sum|count) /{w[$1]=$2}
  /^kestrel_scheduling_latency_seconds_(sum|count) /{s[$1]=$2}
  /^kestrel_detector_stalls_total /{st=$2}
  END{printf "CTRL wal_appends=%d wal_mean_ms=%.3f sched_mean_ms=%.2f detector_stalls=%d\n",
      w["kestrel_wal_append_latency_seconds_count"],
      1000*w["kestrel_wal_append_latency_seconds_sum"]/(w["kestrel_wal_append_latency_seconds_count"]+1e-9),
      1000*s["kestrel_scheduling_latency_seconds_sum"]/(s["kestrel_scheduling_latency_seconds_count"]+1e-9), st}'
