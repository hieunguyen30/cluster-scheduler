#!/usr/bin/env python3
"""Summarise results/exp_b.txt, exp_b2.txt, exp_c.txt (raw lines in, markdown tables out)."""
import re
import statistics as st
import sys
from collections import defaultdict


def parse(path, tag):
    rows = []
    for line in open(path):
        if line.startswith(tag + " "):
            rows.append({k: v for k, v in re.findall(r"(\w+)=(\S+)", line)})
    return rows


def rng(vals):
    vals = [float(v) for v in vals if v != "nan"]
    if not vals:
        return "n/a"
    return f"{st.mean(vals):.2f} ({min(vals):.2f}..{max(vals):.2f})"


def main(d):
    b = parse(f"{d}/exp_b.txt", "EXPB")
    print("### Experiment B: kill K worker nodes (agent + jobs), 24 jobs of 12 s, 9 workers\n")
    print("mean (min..max) over repetitions\n")
    print("| K killed | reps | jobs lost per run | first DEAD after kill (s) | last DEAD after kill (s) | all lost jobs running again (s) | executions (24 jobs) | all 24 finished |")
    print("|---|---|---|---|---|---|---|---|")
    by = defaultdict(list)
    for r in b:
        by[r["k"]].append(r)
    for k in sorted(by):
        rs = by[k]
        fin = sum(int(r["succeeded"]) == int(r["jobs"]) for r in rs)
        print(f"| {k} | {len(rs)} | {rng([r['lost_jobs'] for r in rs])} | {rng([r['detect_first_s'] for r in rs])} | "
              f"{rng([r['detect_last_s'] for r in rs])} | {rng([r['resched_s'] for r in rs])} | "
              f"{rng([r['executions'] for r in rs])} | {fin}/{len(rs)} |")
    b2 = parse(f"{d}/exp_b2.txt", "EXPB2")
    print("\n### Experiment B2: frozen agent (SIGSTOP) then healed (SIGCONT), 12 jobs of 40 s\n")
    print("| reps | jobs on frozen worker | frozen -> DEAD (s) | jobs running twice at once | zombie processes alive before heal | heal -> zombies killed (s) |")
    print("|---|---|---|---|---|---|")
    print(f"| {len(b2)} | {rng([r['jobs_on_frozen_worker'] for r in b2])} | {rng([r['frozen_to_dead_s'] for r in b2])} | "
          f"{rng([r['concurrent_duplicate_jobs'] for r in b2])} | {rng([r['zombie_processes_alive_before_heal'] for r in b2])} | "
          f"{rng([r['heal_to_zombies_killed_s'] for r in b2])} |")
    c = parse(f"{d}/exp_c.txt", "EXPC")
    print("\n### Experiment C: SIGKILL the control plane, restart from the WAL (40 jobs of 10 s, 9 workers)\n")
    print("| reps | WAL size (bytes) | restart -> serving (s) | restart -> all 9 workers ALIVE (s) | jobs lost | jobs executed more than once | jobs with attempts>1 |")
    print("|---|---|---|---|---|---|---|")
    lost = [int(r["jobs"]) - int(r["succeeded"]) for r in c]
    print(f"| {len(c)} | {rng([r['wal_bytes'] for r in c])} | {rng([r['restart_to_serving_s'] for r in c])} | "
          f"{rng([r['restart_to_all_workers_alive_s'] for r in c])} | {lost} | {[int(r['jobs_executed_more_than_once']) for r in c]} | "
          f"{[int(r['jobs_with_attempts_gt1']) for r in c]} |")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "results")
