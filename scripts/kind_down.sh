#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export KUBECONFIG="$ROOT/.tools/kubeconfig"
export PATH="$ROOT/.tools/bin:$PATH"
kind delete cluster --name kestrel || true
