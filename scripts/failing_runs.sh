#!/usr/bin/env bash
# From a gtest_repeat log, print the log lines of the iterations that failed (INFO/WARN included).
# Usage: scripts/failing_runs.sh <logfile> [context_lines]
log="${1:?log}"; ctx="${2:-25}"
grep -nE '\[ RUN +\]|Failure' "$log" | awk -F: '/RUN/{run=$1} /Failure/{print run}' | sort -un | while read -r start; do
  echo "----- iteration starting at line $start -----"
  sed -n "${start},$((start + 400))p" "$log" | grep -E 'to=DEAD|to=SUSPECT|registered|re-register|rejected|Failure|Which is|Expected' | cut -c1-190 | head -"$ctx"
done
