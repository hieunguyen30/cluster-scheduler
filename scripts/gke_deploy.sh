#!/usr/bin/env bash
# Deploy Kestrel to an EXISTING GKE Autopilot cluster. NOT RUN by the assistant: it needs
# credentials and spends money. Usage: scripts/gke_deploy.sh PROJECT REGION IMAGE_TAG
set -euo pipefail
PROJECT="${1:?project}"; REGION="${2:?region}"; TAG="${3:?image tag}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="$REGION-docker.pkg.dev/$PROJECT/kestrel/kestrel:$TAG"
gcloud container clusters get-credentials kestrel --region "$REGION" --project "$PROJECT"
# Render the overlay with the real image reference (no files are modified in the repo).
kubectl kustomize "$ROOT/deploy/k8s/overlays/gke" | sed "s|image: kestrel$|image: $IMAGE|" | kubectl apply -f -
kubectl -n kestrel rollout status statefulset/kestrel-control --timeout=300s
echo "Workers are at 0 replicas. Start the session with:  scripts/gke_scale.sh up"
