#!/usr/bin/env bash
# Scale Kestrel up for a benchmark session or down to zero when idle (Autopilot bills per pod
# request while pods exist). NOT RUN by the assistant. Usage: scripts/gke_scale.sh up [workers_per_size] | down
set -euo pipefail
case "${1:?up|down}" in
  up)
    N="${2:-3}"
    kubectl -n kestrel scale statefulset/kestrel-control --replicas=1
    kubectl -n kestrel rollout status statefulset/kestrel-control --timeout=300s
    for s in small medium large; do kubectl -n kestrel scale deployment/kestrel-worker-$s --replicas="$N"; done ;;
  down)
    for s in small medium large; do kubectl -n kestrel scale deployment/kestrel-worker-$s --replicas=0; done
    kubectl -n kestrel scale statefulset/kestrel-control --replicas=0 ;;   # the PVC (WAL) is kept
  *) exit 2 ;;
esac
kubectl -n kestrel get pods
