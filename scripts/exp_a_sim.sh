#!/usr/bin/env bash
# Experiment A (simulator): placement-policy comparison, 50 seeds per load level.
# Output: results/exp_a_sim_load<L>.csv and .md. Fully determined by the arguments below.
set -euo pipefail
cd "$(dirname "$0")/.."
SIM="${SIM:-$HOME/kestrel-build/plain/src/kestrel_sim}"
mkdir -p results
WORKERS=9; JOBS=2000; SEEDS=50
{
  echo "# Experiment A (simulator) setup"
  echo "- date: $(date -u +%Y-%m-%dT%H:%MZ)  commit: $(git rev-parse --short HEAD)  sim: $(basename "$SIM")"
  echo "- workers=$WORKERS (cycling 2c/8GiB, 4c/16GiB, 8c/32GiB), jobs=$JOBS, seeds 1..$SEEDS, policies rr/ff/bf"
  echo "- build: $(grep -m1 CMAKE_BUILD_TYPE ~/kestrel-build/plain/CMakeCache.txt | cut -d= -f2), compiler: $(c++ --version | head -1)"
} > results/exp_a_sim_setup.md
for LOAD in 0.60 0.85 1.00 1.15; do
  "$SIM" --policy all --first-seed 1 --seeds $SEEDS --workers $WORKERS --jobs $JOBS --load "$LOAD" \
      --csv "results/exp_a_sim_load$LOAD.csv" > "results/exp_a_sim_load$LOAD.txt"
  python3 scripts/analyze_sim.py "results/exp_a_sim_load$LOAD.csv" > "results/exp_a_sim_load$LOAD.md"
  echo "== load $LOAD"; cat "results/exp_a_sim_load$LOAD.md"; echo
done
