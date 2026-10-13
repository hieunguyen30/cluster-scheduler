#!/usr/bin/env bash
# Timeline of the control plane's last start: recovery, registrations, deaths, re-assignments.
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export KUBECONFIG="$ROOT/.tools/kubeconfig"
export PATH="$ROOT/.tools/bin:$PATH"
K="kubectl -n kestrel"
$K logs kestrel-control-0 > /tmp/ctl.log
grep -nE 'wal opened|recovered' /tmp/ctl.log | cut -c1-160
echo "--- registrations after recovery (only workers that reported something)"
grep -E 'worker registered' /tmp/ctl.log | grep -vE 'reported_running=0 kill=0' | cut -c1-175 | head -20
echo "--- worker deaths with live (non-ghost) names, and re-assignments (attempt>=2)"
grep -E 'to=DEAD' /tmp/ctl.log | grep -c .
grep -E 'assigned' /tmp/ctl.log | grep -E 'attempt=[2-9]' | cut -c1-175 | head -20
echo "--- rejected heartbeats / stalls"
grep -cE 'heartbeat rejected' /tmp/ctl.log; grep -E 'stalled' /tmp/ctl.log | head -3
