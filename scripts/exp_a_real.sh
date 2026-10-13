#!/usr/bin/env bash
# Experiment A on the REAL system (local processes): same seeded workload as the simulator,
# time-compressed by SCALE. Output: results/exp_a_real.txt (raw RESULT lines) + setup record.
set -euo pipefail
cd "$(dirname "$0")/.."
SEEDS="${SEEDS:-5}"; JOBS="${JOBS:-300}"; WORKERS=9; LOAD="${LOAD:-0.85}"; SCALE="${SCALE:-5}"; HB=100
mkdir -p results
OUT=results/exp_a_real.txt
{
  echo "# Experiment A (real local processes) setup"
  echo "# date=$(date -u +%Y-%m-%dT%H:%MZ) commit=$(git rev-parse --short HEAD) cpus=$(nproc) seeds=1..$SEEDS jobs=$JOBS workers=$WORKERS load=$LOAD time_scale=$SCALE heartbeat_ms=$HB wal=every"
} > "$OUT"
for seed in $(seq 1 "$SEEDS"); do
  for policy in round-robin first-fit best-fit; do
    echo "run seed=$seed policy=$policy" >&2
    bash scripts/kill_stray.sh >/dev/null
    bash scripts/bench_local.sh "$policy" "$seed" "$JOBS" "$WORKERS" "$LOAD" "$SCALE" "$HB" | grep -E '^(RESULT|CTRL)' | tee -a "$OUT"
  done
done
