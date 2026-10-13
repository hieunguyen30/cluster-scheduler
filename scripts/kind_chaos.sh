#!/usr/bin/env bash
# Chaos smoke test on the local kind cluster (real pods, real PVC):
#   1. 9 workers, 36 jobs (each sleeps ~4s and appends its index to a file ON ITS WORKER'S DISK
#      is not shared, so we count completions via control-plane state instead)
#   2. delete a worker pod that is running jobs        -> jobs must be rescheduled
#   3. delete the control-plane pod mid-run            -> WAL replay on the PVC, workers re-registe
#   4. every job must end SUCCEEDED
# Usage: scripts/kind_chaos.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export KUBECONFIG="$ROOT/.tools/kubeconfig"
export PATH="$ROOT/.tools/bin:$PATH"
K="kubectl -n kestrel"
CTL="$K exec kestrel-control-0 -- kestrelctl"

RUN="$(date +%s)"  # request ids are idempotency keys: re-using them returns the OLD jobs
now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN{printf "%.2f", b-a}'; }
metric() { $K get --raw "/api/v1/namespaces/kestrel/pods/kestrel-control-0:9090/proxy/metrics" 2>/dev/null | awk -v m="$1" '$1==m{print $2}'; }

for s in small medium large; do $K scale deployment/kestrel-worker-$s --replicas=3 >/dev/null; done
for s in small medium large; do $K rollout status deployment/kestrel-worker-$s --timeout=120s >/dev/null; done
for _ in $(seq 1 60); do
  [ "$($CTL workers | grep -c ALIVE)" -ge 9 ] && break; sleep 1
done
echo "workers alive: $($CTL workers | grep -c ALIVE)"

echo "== submitting 36 jobs"
# One exec, loop inside the pod: a `kubectl exec` per job is slower than the jobs themselves.
$K exec kestrel-control-0 -- sh -c \
  "for i in \$(seq 1 36); do kestrelctl submit --cpu 1500 --mem 512 --request-id chaos-$RUN-\$i -- sleep 10 >/dev/null; done"
sleep 2

VICTIM=$($CTL list --state RUNNING | head -1 | sed -E 's/.*worker=([^ ]*) .*/\1/')
case "$VICTIM" in
  kestrel-worker-*) ;;
  *) echo "could not find a worker that is running a job (got '$VICTIM')" >&2; exit 1 ;;
esac
RUNNING_ON_VICTIM=$($CTL list --state RUNNING | grep -c "worker=$VICTIM " || true)
echo "== killing worker pod $VICTIM (running $RUNNING_ON_VICTIM job(s))"
T0=$(now)
$K delete pod "$VICTIM" --grace-period=0 --force >/dev/null 2>&1
for _ in $(seq 1 120); do
  $CTL workers | grep -q "$VICTIM .*DEAD" && break; sleep 0.25
done
echo "[measured] worker pod kill -> DEAD: $(since "$T0") s (suspect 3 s / dead 5 s thresholds)"

echo "after worker kill: lost=$(metric kestrel_jobs_lost_total) rescheduled=$(metric kestrel_jobs_rescheduled_total) (read BEFORE the control-plane restart: counters are in-memory)"
sleep 2
echo "== killing control-plane pod (WAL on a PVC)"
T1=$(now)
$K delete pod kestrel-control-0 --grace-period=0 --force >/dev/null 2>&1
$K wait --for=condition=Ready pod/kestrel-control-0 --timeout=120s >/dev/null
echo "[measured] control pod kill -> Ready again: $(since "$T1") s"
for _ in $(seq 1 120); do
  [ "$($CTL workers | grep -c ALIVE)" -ge 9 ] && break; sleep 0.5
done
echo "[measured] control pod kill -> 9 workers re-registered: $(since "$T1") s"

echo "== waiting for all jobs"
for _ in $(seq 1 180); do
  [ "$($CTL list | grep -cE 'PENDING|SCHEDULED|RUNNING' || true)" -eq 0 ] && break; sleep 1
done
$CTL list | awk '{print $3}' | sort | uniq -c
echo "jobs lost (attempts): $(metric kestrel_jobs_lost_total)  rescheduled: $(metric kestrel_jobs_rescheduled_total)"
echo "jobs with attempts>1: $($CTL list | grep -vc 'attempts=1/' || true)"
echo "detector stalls: $(metric kestrel_detector_stalls_total)"
echo "WAL frames: $(metric kestrel_wal_frames_total)  (this is the NEW process's counter: it counts post-restart appends)"
