#!/usr/bin/env bash
# Fetch pinned helper binaries into the repo-local .tools/bin (git-ignored). Nothing is installed
# system-wide and nothing outside the repo is modified.
#   scripts/tools.sh kind | terraform
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="$ROOT/.tools/bin"
mkdir -p "$BIN"
KIND_VERSION="v0.24.0"
TERRAFORM_VERSION="1.9.8"
case "${1:?kind|terraform}" in
  kind)
    [ -x "$BIN/kind" ] || curl -fsSL -o "$BIN/kind" "https://kind.sigs.k8s.io/dl/$KIND_VERSION/kind-linux-amd64"
    chmod +x "$BIN/kind"; "$BIN/kind" version ;;
  terraform)
    if [ ! -x "$BIN/terraform" ]; then
      tmp="$(mktemp -d)"
      curl -fsSL -o "$tmp/tf.zip" "https://releases.hashicorp.com/terraform/${TERRAFORM_VERSION}/terraform_${TERRAFORM_VERSION}_linux_amd64.zip"
      (cd "$tmp" && (command -v unzip >/dev/null && unzip -q tf.zip || python3 -c 'import zipfile;zipfile.ZipFile("tf.zip").extractall(".")'))
      mv "$tmp/terraform" "$BIN/terraform"; chmod +x "$BIN/terraform"; rm -rf "$tmp"
    fi
    "$BIN/terraform" version | head -1 ;;
  *) echo "unknown tool" >&2; exit 2 ;;
esac
