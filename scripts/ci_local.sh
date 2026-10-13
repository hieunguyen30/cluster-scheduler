#!/usr/bin/env bash
# Run exactly what cloudbuild.yaml runs, locally with Docker (no cloud involved).
set -euo pipefail
cd "$(dirname "$0")/.."
echo "== test-asan (build fails if any test fails)"
docker build --target test-asan -t kestrel-test-asan . 2>&1 | tail -4
echo "== tsan: build image, then run tests with seccomp unconfined"
docker build --target tsan -t kestrel-test-tsan . 2>&1 | tail -2
docker run --rm --security-opt seccomp=unconfined kestrel-test-tsan 2>&1 | tail -6
