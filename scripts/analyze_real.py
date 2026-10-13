#!/usr/bin/env python3
"""Summarise results/exp_a_real.txt (loadgen RESULT lines): per-policy means and paired diffs."""
import math, re, statistics, sys
from collections import defaultdict

KEYS = ["makespan_s", "cpu_util_busy", "mem_util_busy", "cpu_util_makespan", "frag_fraction_busy",
        "queue_wait_p50_s", "queue_wait_p99_s", "retried", "unfinished"]
data = defaultdict(dict)  # key -> policy -> {seed: val}
for line in open(sys.argv[1]):
    if not line.startswith("RESULT"):
        continue
    kv = dict(re.findall(r"(\w+)=(\S+)", line))
    for k in KEYS:
        data[k].setdefault(kv["label"], {})[kv["seed"]] = float(kv[k])
pols = ["round-robin", "first-fit", "best-fit"]
n = len(data[KEYS[0]]["first-fit"])
print(f"{sys.argv[1]}: {n} seeds per policy\n")
print("| metric | " + " | ".join(pols) + " |\n|---|" + "---|" * 3)
for k in KEYS:
    print(f"| {k} | " + " | ".join(f"{statistics.mean(data[k][p].values()):.4g} (sd {statistics.stdev(data[k][p].values()):.2g})" for p in pols) + " |")
print("\nPaired difference vs first-fit (mean, min..max over seeds; n is small, no CI claimed)\n")
print("| metric | round-robin | best-fit |\n|---|---|---|")
for k in KEYS[:-2]:
    cells = []
    for p in ("round-robin", "best-fit"):
        d = [data[k][p][s] - data[k]["first-fit"][s] for s in data[k][p]]
        cells.append(f"{statistics.mean(d):+.4g} ({min(d):+.3g}..{max(d):+.3g})")
    print(f"| {k} | " + " | ".join(cells) + " |")
