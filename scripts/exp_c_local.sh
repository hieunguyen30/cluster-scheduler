#!/usr/bin/env bash
# Experiment C (real processes): SIGKILL the control plane mid-run and restart it from its WAL.
# Measures restart->serving and restart->all workers re-registered, and whether any job is lost o
# executed twice. Usage: scripts/exp_c_local.sh <rep>      prints one EXPC line
set -o pipefail
cd "$(dirname "$0")/.."
source scripts/exp_lib.sh
REP="${1:-1}"
trap cluster_down EXIT
cluster_up wal || exit 1
JOBS=40
submit_batch $JOBS 500 10 "c-$REP"
sleep 4
N_RUN_BEFORE=$($CTL list --state RUNNING | wc -l)
WAL_BYTES=$(stat -c %s "$WORK/kestrel.wal")
kill -9 "$CP_PID"
wait "$CP_PID" 2>/dev/null
T0=$(now)
"$B/kestrel_control" "${CP_ARGS[@]}" 2>>"$WORK/control.log" &
CP_PID=$!
T_SERVE=""; T_ALIVE=""
for _ in $(seq 1 1200); do
  out="$($CTL workers 2>/dev/null)"
  if [ -n "$out" ] && [ -z "$T_SERVE" ]; then T_SERVE="$(elapsed "$T0" "$(now)")"; fi
  if [ "$(echo "$out" | grep -c ALIVE)" -ge 9 ]; then T_ALIVE="$(elapsed "$T0" "$(now)")"; break; fi
  sleep 0.05
done
for _ in $(seq 1 240); do all_terminal && break; sleep 0.5; done
SUCC=$($CTL list | grep -c SUCCEEDED)
TOTAL=$($CTL list | grep -c '^job')
RETRIED=$($CTL list | grep -vc 'attempts=1/' || true)
EXEC=$(wc -l < "$RUNS")
DUP=$(awk '{print $1}' "$RUNS" | sort | uniq -d | wc -l)
echo "EXPC rep=$REP jobs=$JOBS running_before_kill=$N_RUN_BEFORE wal_bytes=$WAL_BYTES restart_to_serving_s=${T_SERVE:-nan} restart_to_all_workers_alive_s=${T_ALIVE:-nan} jobs_in_table=$TOTAL succeeded=$SUCC jobs_with_attempts_gt1=$RETRIED executions=$EXEC jobs_executed_more_than_once=$DUP"
