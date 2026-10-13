#!/usr/bin/env bash
cd "$(dirname "$0")/.."
python3 scripts/analyze_real.py results/exp_a_real.txt | tee results/exp_a_real.md
head -2 results/exp_a_real.txt
echo "--- control-plane metrics per run (CTRL lines): wal_mean_ms / detector_stalls"
grep CTRL results/exp_a_real.txt | sed -E 's/.*wal_appends=([0-9]+) wal_mean_ms=([0-9.]+).*detector_stalls=([0-9]+)/\1 \2 \3/' | awk '{s+=$2; n++; if($2>m)m=$2; st+=$3} END{printf "runs=%d wal_mean_of_means=%.2f ms max_of_means=%.2f ms total_detector_stalls=%d\n", n, s/n, m, st}'
grep -c '^RESULT' results/exp_a_real.txt
grep '^RESULT' results/exp_a_real.txt | grep -vE 'unfinished=0 retried=0' | head -5
