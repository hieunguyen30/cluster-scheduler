#!/usr/bin/env bash
cd "$(dirname "$0")/.."
cmake --build "$HOME/kestrel-build/plain" --target kestrel_walbench -j8 >/dev/null 2>&1 || { echo "build failed"; exit 1; }
{
  echo "# WAL micro-benchmark: date=$(date -u +%Y-%m-%dT%H:%MZ) commit=$(git rev-parse --short HEAD) kernel=$(uname -r)"
  echo "# filesystem of /tmp: $(df -T /tmp | tail -1 | awk '{print $2" on "$1}')"
  for batch in 1 10; do
    "$HOME/kestrel-build/plain/src/kestrel_walbench" --dir /tmp --records 3000 --batch "$batch"
  done
} | tee results/walbench.txt
