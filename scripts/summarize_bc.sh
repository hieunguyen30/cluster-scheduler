#!/usr/bin/env bash
cd "$(dirname "$0")/.."
python3 scripts/analyze_bc.py results | tee results/exp_bc_summary.md
echo; echo "--- raw lines with anomalies (partial finish / duplicates / nan):"
grep -hE 'nan|succeeded=([0-9]+) ' results/exp_b.txt results/exp_c.txt | awk '/nan/ {print}' | head
