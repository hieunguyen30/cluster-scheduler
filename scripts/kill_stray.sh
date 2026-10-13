#!/usr/bin/env bash
# Kill stray Kestrel binaries left behind by an interrupted demo/benchmark run. Matches only the
# binaries (never script names), so it cannot kill its own caller.
for pat in '[k]estrel_control' '[k]estrel_worker' '[k]estrel_loadgen'; do
  pkill -f "$pat" 2>/dev/null || true
done
sleep 0.5
left=$(pgrep -fa '[k]estrel_(control|worker|loadgen)' | wc -l)
echo "stray kestrel processes remaining: $left"
