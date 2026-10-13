#!/usr/bin/env bash
# Just the TSan stage of cloudbuild.yaml, locally: build image, then run with seccomp unconfined.
set -uo pipefail
cd "$(dirname "$0")/.."
docker build --target tsan -t kestrel-test-tsan . > /tmp/ci_tsan_build.log 2>&1 || { echo "BUILD FAILED"; tail -15 /tmp/ci_tsan_build.log; exit 1; }
docker run --rm --security-opt seccomp=unconfined kestrel-test-tsan > /tmp/ci_tsan_run.log 2>&1
echo "docker run exit=$?"
grep -E 'tests passed|tests failed|\*\*\*Failed|WARNING: ThreadSanitizer' /tmp/ci_tsan_run.log | sort | uniq -c | head
