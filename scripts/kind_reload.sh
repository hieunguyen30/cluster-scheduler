#!/usr/bin/env bash
# Rebuild the image, load it into kind and restart all Kestrel pods. Usage: scripts/kind_reload.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export KUBECONFIG="$ROOT/.tools/kubeconfig"
export PATH="$ROOT/.tools/bin:$PATH"
docker build --target runtime -t kestrel:dev "$ROOT" | tail -1
kind load docker-image kestrel:dev --name kestrel
kubectl -n kestrel rollout restart statefulset/kestrel-control deployment/kestrel-worker-small \
    deployment/kestrel-worker-medium deployment/kestrel-worker-large
kubectl -n kestrel rollout status statefulset/kestrel-control --timeout=180s
for s in small medium large; do kubectl -n kestrel rollout status deployment/kestrel-worker-$s --timeout=180s; done
