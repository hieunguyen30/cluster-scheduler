# Kestrel

A small Borg/Kubernetes-inspired cluster scheduler in C++20: jobs with CPU/memory requests are
placed on workers by a pluggable bin-packing policy; dead workers are detected by heartbeat and
their jobs rescheduled; the control plane survives restarts via a write-ahead log. See
[DESIGN.md](DESIGN.md) for the design, [DECISIONS.md](DECISIONS.md) for the reasoning and
[INTERVIEW_PREP.md](INTERVIEW_PREP.md) for explanations.

Status: **Phase 1 done** (core + simulator). Later phases are added as they land.

## Build and test (WSL2 Ubuntu 24.04)

Requirements: `build-essential clang cmake ninja-build pkg-config libgrpc++-dev libprotobuf-dev
protobuf-compiler-grpc`.

```bash
scripts/build.sh plain     # build + ctest        (build dir: ~/kestrel-build/plain)
scripts/build.sh asan      # AddressSanitizer + UBSan
scripts/build.sh tsan      # ThreadSanitizer
```

## Policy comparison simulator

```bash
~/kestrel-build/plain/src/kestrel_sim --policy all --first-seed 1 --seeds 10 \
    --workers 10 --jobs 2000 --load 0.85 --check --csv out.csv
```
Prints the full configuration first. Results are in the "Benchmark results" section when run
for the report.

## Known limitations

- Single control plane (no HA), one coarse global lock; WAL replay on restart can be slow for
  large logs.
- CPU/memory are logical reservations, not enforced limits.
- FIFO queue with head-of-line blocking (no backfill).
- At-least-once execution: jobs must be idempotent.
- Failure-detection thresholds are static, not adaptive.
- Simulator: inter-arrival gaps are uniform, not exponential (less bursty than Poisson);
  fragmentation is measured for the queue head only; equal CPU/memory weighting in best-fit;
  results are bit-identical on one toolchain, not verified across toolchains.
