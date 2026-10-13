#!/usr/bin/env bash
echo "--- ci_local.log tail"; tail -12 /tmp/ci_local.log 2>/dev/null
echo "--- compilers/builds running:"; pgrep -fa 'cc1plus|docker build|ninja' | cut -c1-120 | head -5
echo "--- load:"; cat /proc/loadavg
echo "--- exp_a_real progress:"; grep -c '^RESULT' /mnt/c/Users/09041/Documents/PersonalProject/cluster-scheduler/cluster-scheduler/results/exp_a_real.txt 2>/dev/null
