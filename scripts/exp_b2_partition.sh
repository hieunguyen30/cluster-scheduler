#!/usr/bin/env bash
# Experiment B2 (real processes): a worker agent is FROZEN (SIGSTOP = a hung/partitioned agent whose
# jobs keep running). The control plane declares it dead and reruns its jobs elsewhere, so jobs
# execute twice at once (at-least-once). SIGCONT = partition heals; fencing must kill the zombies.
# Usage: scripts/exp_b2_partition.sh <rep>      prints one EXPB2 line
set -o pipefail
cd "$(dirname "$0")/.."
source scripts/exp_lib.sh
REP="${1:-1}"
trap cluster_down EXIT
cluster_up wal || exit 1
JOBS=12
submit_batch $JOBS 500 40 "b2-$REP"
sleep 3
V=$($CTL list --state RUNNING | sed -E 's/.*worker=([^ ]*) .*/\1/' | sort -u | head -1)
IDX=${V#w}
N_ON_V=$($CTL list --state RUNNING | grep -c "worker=$V ")
# job processes currently running on the victim = children of its agent
ZOMBIES=$(pgrep -P "${WORKER_PIDS[$IDX]}" | tr '\n' ' ')
kill -STOP "${WORKER_PIDS[$IDX]}"
T0=$(now)
for _ in $(seq 1 400); do
  $CTL workers | grep -q " $V .*DEAD" && break
  sleep 0.05
done
T_DEAD=$(elapsed "$T0" "$(now)")
# wait until the frozen worker's jobs have been started a second time somewhere else
for _ in $(seq 1 400); do
  [ "$(awk '{print $1}' "$RUNS" | sort | uniq -d | wc -l)" -ge "$N_ON_V" ] && break
  sleep 0.05
done
DUP_JOBS=$(awk '{print $1}' "$RUNS" | sort | uniq -d | wc -l)
ALIVE_ZOMBIES=0
for z in $ZOMBIES; do kill -0 "$z" 2>/dev/null && ALIVE_ZOMBIES=$((ALIVE_ZOMBIES + 1)); done
kill -CONT "${WORKER_PIDS[$IDX]}"
T1=$(now)
for _ in $(seq 1 400); do
  left=0
  for z in $ZOMBIES; do kill -0 "$z" 2>/dev/null && left=$((left + 1)); done
  [ "$left" -eq 0 ] && break
  sleep 0.05
done
T_FENCE=$(elapsed "$T1" "$(now)")
for id in $(seq 1 $JOBS); do $CTL cancel "$id" >/dev/null 2>&1; done
echo "EXPB2 rep=$REP jobs=$JOBS jobs_on_frozen_worker=$N_ON_V frozen_to_dead_s=$T_DEAD concurrent_duplicate_jobs=$DUP_JOBS zombie_processes_alive_before_heal=$ALIVE_ZOMBIES heal_to_zombies_killed_s=$T_FENCE"
