#!/usr/bin/env bash
# Offline Terraform checks only: init (downloads the provider plugin, no cloud calls),
# fmt and validate. NEVER runs plan/apply: those talk to Google Cloud.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TF="$ROOT/.tools/bin/terraform"
cd "$ROOT/terraform"
"$TF" init -backend=false -input=false >/dev/null
"$TF" fmt -check -recursive && echo "fmt: ok"
"$TF" validate
