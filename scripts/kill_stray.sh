#!/usr/bin/env bash
# Kill stray Kestrel processes left behind by an interrupted demo/benchmark run.
for pat in '[b]ench_local.sh' '[d]emo_local.sh' '[d]emo_restart.sh' '[k]estrel_control' '[k]estrel_worker' '[k]estrel_loadgen'; do
  pkill -f "$pat" 2>/dev/null || true
done
sleep 0.5
left=$(pgrep -fa '[k]estrel_(control|worker|loadgen)' | wc -l)
echo "stray kestrel processes remaining: $left"
