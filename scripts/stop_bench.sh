#!/usr/bin/env bash
# Stop a running exp_* benchmark and its Kestrel processes.
pkill -f '[e]xp_a_real.sh' 2>/dev/null || true
pkill -f '[b]ench_local.sh' 2>/dev/null || true
bash "$(dirname "$0")/kill_stray.sh"
