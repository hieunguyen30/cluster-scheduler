#!/usr/bin/env bash
# Experiment B (real processes): kill K worker "nodes" (agent + its jobs, SIGKILL on the process
# group) mid-run. Measures detection time, time until every lost job is running again, number of
# rescheduled jobs, executions, and whether every job finishes.
# Usage: scripts/exp_b_local.sh <K> <rep>      prints one EXPB line
set -o pipefail   # not -u: empty associative arrays trip it on bash 5.2
cd "$(dirname "$0")/.."
source scripts/exp_lib.sh
K="${1:?k}"; REP="${2:-1}"
trap cluster_down EXIT
cluster_up wal || exit 1

JOBS=24
submit_batch $JOBS 500 12 "b$K-$REP"
sleep 3   # let every job be assigned and running

# victims: the first K distinct workers that currently run a job
mapfile -t VICTIMS < <($CTL list --state RUNNING | sed -E 's/.*worker=([^ ]*) .*/\1/' | sort -u | head -n "$K")
LOST_IDS=""
for v in "${VICTIMS[@]}"; do
  LOST_IDS="$LOST_IDS $($CTL list --state RUNNING | grep "worker=$v " | awk '{print $2}' | tr '\n' ' ')"
done
N_LOST=$(echo $LOST_IDS | wc -w)

T0=$(now)
for v in "${VICTIMS[@]}"; do
  idx="${v#w}"
  kill -9 -- "-${WORKER_PIDS[$idx]}" 2>/dev/null
done

declare -A TDEAD
T_RESCHED=""
for _ in $(seq 1 1200); do   # up to ~60 s at 50 ms
  out="$($CTL workers 2>/dev/null)"
  for v in "${VICTIMS[@]}"; do
    if [ -z "${TDEAD[$v]:-}" ] && echo "$out" | grep -q " $v .*DEAD"; then TDEAD[$v]="$(elapsed "$T0" "$(now)")"; fi
  done
  if [ -z "$T_RESCHED" ] && [ "${#TDEAD[@]}" -eq "$K" ]; then
    ok=1
    for id in $LOST_IDS; do
      line="$($CTL get "$id" 2>/dev/null)"
      st=$(echo "$line" | awk '{print $3}')
      at=$(echo "$line" | sed -E 's/.*attempts=([0-9]+)\/.*/\1/')
      case "$st" in
        RUNNING|SUCCEEDED) [ "$at" -ge 2 ] || ok=0 ;;
        *) ok=0 ;;
      esac
    done
    [ "$ok" = 1 ] && T_RESCHED="$(elapsed "$T0" "$(now)")"
  fi
  [ -n "$T_RESCHED" ] && break
  sleep 0.05
done
DEAD_FIRST=$(for v in "${VICTIMS[@]}"; do echo "${TDEAD[$v]:-nan}"; done | sort -n | head -1)
DEAD_LAST=$(for v in "${VICTIMS[@]}"; do echo "${TDEAD[$v]:-nan}"; done | sort -n | tail -1)

for _ in $(seq 1 240); do all_terminal && break; sleep 0.5; done
SUCC=$($CTL list | grep -c SUCCEEDED)
ATT2=$($CTL list | grep -vc 'attempts=1/' || true)
EXEC=$(wc -l < "$RUNS")
DISTINCT=$(awk '{print $1}' "$RUNS" | sort -u | wc -l)
echo "EXPB k=$K rep=$REP jobs=$JOBS lost_jobs=$N_LOST detect_first_s=$DEAD_FIRST detect_last_s=$DEAD_LAST resched_s=${T_RESCHED:-nan} succeeded=$SUCC jobs_with_attempts_gt1=$ATT2 executions=$EXEC distinct_jobs_executed=$DISTINCT"
