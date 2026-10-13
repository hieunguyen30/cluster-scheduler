#!/usr/bin/env bash
# Create a local kind cluster, load the image and deploy Kestrel. Usage: scripts/kind_up.sh
# Uses a repo-local kubeconfig (.tools/kubeconfig) so ~/.kube/config is never touched.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export KUBECONFIG="$ROOT/.tools/kubeconfig"
export PATH="$ROOT/.tools/bin:$PATH"
bash "$ROOT/scripts/tools.sh" kind >/dev/null
if ! kind get clusters 2>/dev/null | grep -qx kestrel; then
  kind create cluster --config "$ROOT/deploy/kind/kind-config.yaml" --kubeconfig "$KUBECONFIG" --wait 120s
fi
kind load docker-image kestrel:dev --name kestrel
kubectl apply -k "$ROOT/deploy/k8s/overlays/kind"
kubectl -n kestrel rollout status statefulset/kestrel-control --timeout=180s
for d in small medium large; do kubectl -n kestrel rollout status deployment/kestrel-worker-$d --timeout=180s; done
kubectl -n kestrel get pods -o wide
