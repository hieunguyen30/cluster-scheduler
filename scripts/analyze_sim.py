#!/usr/bin/env python3
"""Summarise kestrel_sim CSV output: per-policy means and PAIRED differences between policies.

Every seed gives all three policies the identical job stream, so the right comparison is the
per-seed difference, not the difference of means. We report mean difference, a 95% CI
(Student-t, Cornish-Fisher approximation of the critical value) and win/loss/tie counts.
Usage: analyze_sim.py file.csv [baseline_policy=first-fit]
"""
import csv
import math
import statistics
import sys
from collections import defaultdict

METRICS = [
    ("makespan_s", "makespan (s)", False),
    ("cpu_util_busy", "CPU util while queue non-empty", True),
    ("mem_util_busy", "mem util while queue non-empty", True),
    ("cpu_util_makespan", "CPU util over makespan", True),
    ("frag_fraction_busy", "fragmentation (frac of busy time)", False),
    ("wait_p50_s", "queue wait p50 (s)", False),
    ("wait_p99_s", "queue wait p99 (s)", False),
]


def t_crit(df: int) -> float:
    z = statistics.NormalDist().inv_cdf(0.975)
    return z + (z**3 + z) / (4 * df) + (5 * z**5 + 16 * z**3 + 3 * z) / (96 * df**2)


def main() -> None:
    path = sys.argv[1]
    base = sys.argv[2] if len(sys.argv) > 2 else "first-fit"
    rows = list(csv.DictReader(open(path)))
    by = defaultdict(dict)  # metric -> policy -> {seed: value}
    policies = []
    for r in rows:
        p = r["policy"]
        if p not in policies:
            policies.append(p)
        for key, _, _ in METRICS:
            by[key].setdefault(p, {})[r["seed"]] = float(r[key])
    n = len(next(iter(by[METRICS[0][0]].values())))
    print(f"{path}: {n} seeds, workers={rows[0]['workers']}, jobs={rows[0]['jobs']}, load={rows[0]['load']}")
    print()
    print("| metric | " + " | ".join(policies) + " |")
    print("|---|" + "---|" * len(policies))
    for key, label, _ in METRICS:
        cells = []
        for p in policies:
            v = list(by[key][p].values())
            cells.append(f"{statistics.mean(v):.4g} (sd {statistics.stdev(v):.2g})")
        print(f"| {label} | " + " | ".join(cells) + " |")
    print()
    print(f"Paired difference vs **{base}** (policy minus {base}, mean +/- 95% CI, wins/losses/ties by seed)")
    print("A 'win' is the better direction for that metric. Differences whose CI contains 0 are not distinguishable from noise at this n.")
    print()
    print("| metric | " + " | ".join(p for p in policies if p != base) + " |")
    print("|---|" + "---|" * (len(policies) - 1))
    for key, label, higher_better in METRICS:
        cells = []
        for p in policies:
            if p == base:
                continue
            d = [by[key][p][s] - by[key][base][s] for s in by[key][p]]
            m = statistics.mean(d)
            half = t_crit(len(d) - 1) * statistics.stdev(d) / math.sqrt(len(d)) if len(d) > 1 else float("nan")
            eps = 1e-9 + 1e-6 * abs(statistics.mean(by[key][base].values()))
            better = (lambda x: x > eps) if higher_better else (lambda x: x < -eps)
            worse = (lambda x: x < -eps) if higher_better else (lambda x: x > eps)
            w = sum(better(x) for x in d)
            l = sum(worse(x) for x in d)
            cells.append(f"{m:+.4g} +/- {half:.2g} ({w}/{l}/{len(d) - w - l})")
        print(f"| {label} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main()
