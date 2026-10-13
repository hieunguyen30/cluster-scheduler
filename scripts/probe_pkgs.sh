#!/usr/bin/env bash
# Which runtime packages provide the shared libs our binaries need?
B="${1:-$HOME/kestrel-build/plain/src}"
for bin in kestrel_control kestrel_worker kestrelctl kestrel_loadgen; do
  ldd "$B/$bin" | awk '/=> \//{print $3}'
done | sort -u > /tmp/libs.txt
wc -l < /tmp/libs.txt
# map each lib file to its dpkg package
xargs -a /tmp/libs.txt dpkg -S 2>/dev/null | cut -d: -f1 | sort -u | tr '\n' ' '
echo
