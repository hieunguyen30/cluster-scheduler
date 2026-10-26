# Shared helpers for exp_b/exp_c: start a real local cluster with the default timings
# (heartbeat 1 s, SUSPECT 3 s, DEAD 5 s). Source this file; do not execute it.
B="${KESTREL_BIN:-$HOME/kestrel-build/plain/src}"
now() { date +%s.%N; }
elapsed() { awk -v a="$1" -v b="$2" 'BEGIN{printf "%.3f", b-a}'; }

cluster_up() {  # cluster_up <wal|nowal> ; sets PORT WORK CP_PID CTL RUNS and WORKER_PIDS[]
  PORT="${PORT:-$((20000 + RANDOM % 20000))}"
  WORK="$(mktemp -d)"; RUNS="$WORK/runs.txt"; : > "$RUNS"
  CTL="$B/kestrelctl --control 127.0.0.1:$PORT"
  CP_ARGS=(--listen 127.0.0.1:$PORT --metrics-port 0 --log-level warn)
  [ "${1:-wal}" = wal ] && CP_ARGS+=(--wal-path "$WORK/kestrel.wal" --wal-sync every)
  "$B/kestrel_control" "${CP_ARGS[@]}" 2>>"$WORK/control.log" &
  CP_PID=$!
  sleep 0.6
  cpus=(2000 4000 8000); mems=(8192 16384 32768); WORKER_PIDS=()
  for i in $(seq 0 8); do
    # setsid: each agent leads its own process group, so one `kill -9 -PGID` is a "node failure"
    # that takes the agent AND its job processes down together.
    setsid "$B/kestrel_worker" --control 127.0.0.1:$PORT --name "w$i" --cpu-millis "${cpus[$((i % 3))]}" \
        --mem-mib "${mems[$((i % 3))]}" --heartbeat-ms 1000 --log-level warn 2>"$WORK/w$i.log" &
    WORKER_PIDS+=($!)
  done
  for _ in $(seq 1 100); do
    [ "$($CTL workers 2>/dev/null | grep -c ALIVE)" -ge 9 ] && return 0
    sleep 0.1
  done
  echo "cluster did not come up" >&2
  return 1
}

cluster_down() {
  for p in "${WORKER_PIDS[@]:-}"; do kill -9 -- "-$p" 2>/dev/null || kill -9 "$p" 2>/dev/null || true; done
  kill -9 "${CP_PID:-0}" 2>/dev/null || true
  wait 2>/dev/null || true
  rm -rf "${WORK:-/nonexistent}"
}

# submit_batch <count> <cpu_millis> <sleep_seconds> <prefix>: each job appends "<index> <pid>" to $RUNS
submit_batch() {
  local n="$1" cpu="$2" secs="$3" prefix="$4"
  for i in $(seq 1 "$n"); do
    $CTL submit --cpu "$cpu" --mem 128 --attempts 5 --request-id "$prefix-$i" -- \
        sh -c "echo $i \$\$ >> $RUNS; sleep $secs" >/dev/null
  done
}

all_terminal() { [ "$($CTL list 2>/dev/null | grep -cE 'PENDING|SCHEDULED|RUNNING')" -eq 0 ]; }
