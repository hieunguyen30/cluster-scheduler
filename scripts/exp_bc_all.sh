#!/usr/bin/env bash
# Run experiments B (K=1,2,3), B2 and C, 5 repetitions each, on real local processes with the
# default timings. Raw lines go to results/exp_b.txt, exp_b2.txt, exp_c.txt.
cd "$(dirname "$0")/.."
mkdir -p results
REPS="${REPS:-5}"
hdr() { echo "# $1 setup: date=$(date -u +%Y-%m-%dT%H:%MZ) commit=$(git rev-parse --short HEAD) cpus=$(nproc) workers=9 heartbeat_ms=1000 suspect_after_ms=3000 dead_after_ms=5000 wal=every"; }
hdr "Experiment B (kill K worker nodes)" > results/exp_b.txt
hdr "Experiment B2 (frozen agent: duplicate execution + fencing)" > results/exp_b2.txt
hdr "Experiment C (control plane kill -9 + WAL restart)" > results/exp_c.txt
for rep in $(seq 1 "$REPS"); do
  for k in 1 2 3; do
    bash scripts/kill_stray.sh >/dev/null
    bash scripts/exp_b_local.sh "$k" "$rep" 2>/dev/null | grep '^EXPB' | tee -a results/exp_b.txt
  done
  bash scripts/kill_stray.sh >/dev/null
  bash scripts/exp_b2_partition.sh "$rep" 2>/dev/null | grep '^EXPB2' | tee -a results/exp_b2.txt
  bash scripts/kill_stray.sh >/dev/null
  bash scripts/exp_c_local.sh "$rep" 2>/dev/null | grep '^EXPC' | tee -a results/exp_c.txt
done
echo ALL-DONE
