# Kestrel

A small Borg/Kubernetes-inspired cluster scheduler in C++20. Clients submit jobs with CPU/memory
requests; a control plane places them on worker agents with a pluggable bin-packing policy
(round-robin, first-fit, best-fit); dead workers are detected by heartbeat and their jobs are
rescheduled (at-least-once, with epoch/generation fencing); a write-ahead log lets the control plane
survive a crash. It builds into one container image and has Kubernetes manifests, Terraform and a
Cloud Build pipeline for GKE Autopilot.

* [DESIGN.md](DESIGN.md): the original design. [DECISIONS.md](DECISIONS.md): every non-obvious
  decision, alternatives, bugs found, deviations from the design.
* [INTERVIEW_PREP.md](INTERVIEW_PREP.md): how each piece works, trade-offs, and the answers to the
  eight questions in DESIGN.md section 13.
* [docs/GKE.md](docs/GKE.md): GKE runbook, expected Terraform plan, cost caveats, teardown command.

**What has and has not been run.** Everything below was built and measured on one Windows 11
machine under WSL2 (Ubuntu 24.04, 14 cores, GCC 13, gRPC 1.51 from apt). Containers and manifests
were exercised on a local `kind` cluster. **Nothing has been applied to Google Cloud**: Terraform is
validated offline (`terraform validate`) but was never planned or applied, Cloud Build never ran,
and there are no GKE measurements.

## Architecture

```
          kestrelctl / kestrel_loadgen            gRPC: SubmitJob GetJob ListJobs CancelJob ListWorkers
                      |
                      v
   +--------------------------------------------+      WAL file (PVC)
   | kestrel_control  (single process)          |<---> append-only, CRC32C frames, fdatasync
   |  one coarse mutex around:                  |
   |   ClusterState  jobs, workers, FIFO queue  |      /metrics (Prometheus text) :9090
   |   Scheduler + PlacementPolicy              |
   |  threads: gRPC handlers, scheduler,        |
   |           failure detector                 |
   +--------------------------------------------+
        ^ Register / Heartbeat (worker -> control, replies carry assignments, kill orders, acks)
        |            |            |
   +---------+  +---------+  +---------+
   | worker  |  | worker  |  | worker  |   kestrel_worker: runs each job as a subprocess
   +---------+  +---------+  +---------+   (own process group), re-sends results until acked
```

* **Core** (`src/core`, static library shared by the simulator and the control plane): job/worker
  model, `ClusterState` (every mutation is a `Record` journaled *before* it is applied; replay uses
  the same `Apply`), `PlacementPolicy` implementations, FIFO `Scheduler`.
* **Simulator** (`src/sim`): virtual-time discrete-event simulation (integer microseconds, events
  ordered by `(time, sequence)`, hand-written sampling on `std::mt19937_64`) driving the real core.
* **Control plane** (`src/control`): gRPC services, scheduler and detector threads, metrics.
* **WAL** (`src/wal`): `frame := len | crc(len) | crc(payload) | payload`, one frame per batch.
* **Worker** (`src/worker`): registration, heartbeats, `posix_spawn`, reaping.
* **Failure model**: SUSPECT after 3 s of silence (no new placements), DEAD after 5 s (jobs requeued
  at the front of the queue). Each registration gets a fresh epoch (fences zombie workers); each
  assignment a fresh generation (fences zombie results). Control-plane restart: replay the WAL,
  invalidate all epochs, workers re-register reporting what they run, jobs are adopted or killed.

## Build, test, run

```bash
# WSL2 Ubuntu 24.04:
sudo apt-get install -y build-essential clang cmake ninja-build pkg-config \
     libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc python3
scripts/build.sh plain       # build into ~/kestrel-build/plain and run ctest
scripts/build.sh asan        # AddressSanitizer + UBSan
scripts/build.sh tsan        # ThreadSanitizer (see caveats below)

scripts/demo_local.sh        # control plane + 2 workers + CLI on this machine
scripts/demo_restart.sh      # kill -9 the control plane mid-run, restart from the WAL
scripts/kind_up.sh           # local Kubernetes (kind) with the real manifests, then
scripts/kind_chaos.sh        #   kill a worker pod, then the control pod
```

Simulator: `~/kestrel-build/plain/src/kestrel_sim --policy all --seeds 50 --workers 9 --jobs 2000 --load 1.0 --csv out.csv`
(prints its full configuration first; `scripts/analyze_sim.py` computes paired comparisons).

**Tests: 117, all passing in plain, ASan+UBSan and TSan builds** (unit tests for every component,
property/sweep tests for the WAL, in-process integration tests with real gRPC and real subprocesses,
failure tests: kill workers, partition workers, fence zombies, restart the control plane).
Sanitizer caveats, honestly: TSan needs `setarch -R` on this kernel and `seccomp=unconfined` in
Docker; the distro gRPC is not TSan-instrumented, so TSan runs with
`ignore_noninstrumented_modules=1` plus three narrow symbol suppressions
([tests/tsan.supp](tests/tsan.supp)). It reports no race in Kestrel's own code, but it cannot see
inside gRPC. I verified TSan still catches seeded races under those options. Both CI Docker stages
of `cloudbuild.yaml` (ASan build-and-test, TSan run with `seccomp=unconfined`) pass locally
(`scripts/ci_local.sh`, `scripts/ci_tsan_local.sh`); they have not run on Cloud Build.

## Results

All numbers below are measured, from the raw files in [results/](results/), with the setup recorded
at the top of each file. Seeds are fixed; the simulator is bit-reproducible *on the same
toolchain* (the PRNG stream is toolchain-independent, but floating-point metrics were not compared
across toolchains).

### Experiment A: placement policy (simulator, 50 seeds x 2000 jobs, 9 heterogeneous workers)

Workers cycle 2c/8GiB, 4c/16GiB, 8c/32GiB. Job mix: small 40%, medium 25%, large 10%, cpu-heavy 12%,
mem-heavy 13%. "Load" is the offered load against the more constrained resource (CPU here).
Differences are **paired by seed** (every policy sees the identical job stream), policy minus
first-fit, mean +/- 95% CI. Queue wait = submit to first assignment (virtual time).

| load | policy | makespan (s) | CPU util while queue non-empty | fragmentation (frac of busy time) | queue wait p50 (s) | queue wait p99 (s) |
|---|---|---|---|---|---|---|
| 0.85 | round-robin | +1.15 +/- 0.49 | -0.021 +/- 0.003 | +0.079 +/- 0.011 | +0.96 +/- 0.32 | +1.36 +/- 0.38 |
| 0.85 | best-fit | -0.39 +/- 0.21 | +0.013 +/- 0.002 | -0.039 +/- 0.011 | -0.30 +/- 0.11 | -0.66 +/- 0.20 |
| 1.00 | round-robin | +4.65 +/- 0.70 | -0.015 +/- 0.002 | +0.079 +/- 0.010 | +2.71 +/- 0.51 | +4.42 +/- 0.72 |
| 1.00 | best-fit | -2.59 +/- 0.58 | +0.008 +/- 0.002 | -0.031 +/- 0.009 | -1.29 +/- 0.39 | -2.40 +/- 0.54 |
| 1.15 | round-robin | +4.90 +/- 0.80 | -0.015 +/- 0.003 | +0.081 +/- 0.009 | +2.66 +/- 0.59 | +4.78 +/- 0.81 |
| 1.15 | best-fit | -2.73 +/- 0.58 | +0.009 +/- 0.002 | -0.032 +/- 0.011 | -1.34 +/- 0.38 | -2.69 +/- 0.57 |

Absolute levels at load 1.0 (means): makespan 316 / 311 / 309 s, CPU utilisation while queued
0.891 / 0.906 / 0.914, p99 wait 33.4 / 29.0 / 26.6 s for round-robin / first-fit / best-fit.

* **Best-fit beats first-fit beats round-robin at load >= 0.85, and the paired CIs exclude zero,
  but the effect is small**: about +0.8 to +1.3 points of CPU utilisation, 0.1-0.9% shorter makespan
  (0.12% at load 0.85, 0.83% at 1.0, 0.88% at 1.15), 5-11% lower p99 queue wait. Best-fit won on
  CPU utilisation in 45-48 of 50 seeds.
* **At load 0.6 differences are mostly noise** (the queue is non-empty only a few percent of the
  time, so the "while queued" ratios are erratic); see `results/exp_a_sim_load0.60.md`.
* Memory is far from binding in this mix (60% utilised while CPU is ~90%), so the memory half of
  the best-fit score is barely exercised. Fragmentation is high for every policy (80-95% of busy
  time the head job is blocked although aggregate free capacity would fit it): FIFO head-of-line
  blocking, not placement, is the dominant inefficiency.
* Makespan at low load is arrival-bound and says nothing about placement.

### Experiment A on the real system (9 local worker processes, WAL on, 8 seeds x 400 jobs, load 1.0)

Same seeded workload, durations compressed 5x, heartbeat 100 ms, WAL `fdatasync` on every
mutation. Means over 8 seeds (sd in `results/exp_a_real.md`); n is too small for confidence
intervals, so only per-seed ranges are shown.

| metric | round-robin | first-fit | best-fit |
|---|---|---|---|
| CPU util while queue non-empty | 0.871 | 0.900 | 0.907 |
| makespan (s) | 16.97 | 17.01 | 16.42 |
| queue wait p50 (s) | 1.18 | 1.00 | 0.93 |
| queue wait p99 (s) | 2.93 | 2.65 | 2.68 |

* Round-robin is clearly worst on CPU utilisation (-0.029 vs first-fit, range -0.063..+0.007).
* **Best-fit vs first-fit is NOT distinguishable here**: per-seed differences straddle zero
  (CPU util +0.007, range -0.016..+0.028; p99 wait +0.03 s, range -1.16..+1.13 s). The direction
  matches the simulator but this experiment cannot confirm it.
* Overheads the simulator does not have: mean WAL append 3.6 ms (max 4.0 ms of per-run means), and
  the control plane logged 21 stall events over the 24 runs (slow fsync under the global lock; see
  DECISIONS D36).

### Experiment B: worker failure (9 local worker processes, DESIGN.md timing: heartbeat 1 s, SUSPECT 3 s, DEAD 5 s)

24 jobs of 12 s; K "nodes" killed with `SIGKILL` of the agent's process group (the agent and its
jobs die together). 5 repetitions per K; mean (min..max). Source: `results/exp_b.txt`.

| K | jobs lost | first DEAD after kill (s) | all lost jobs running again (s) | executions of 24 jobs | all 24 finished |
|---|---|---|---|---|---|
| 1 | 4.0 | 4.61 (4.53..4.73) | 6.61 (6.50..6.74) | 28.0 | 5/5 |
| 2 | 8.8 (8..12) | 5.05 (4.54..5.84) | 7.14 (6.60..7.96) | 32.8 | 5/5 |
| 3 | 15.2 (12..16) | 5.12 (4.56..5.95) | 7.62 (6.67..8.36) | 39.2 | 5/5 |

* Detection is "dead_after minus time since the victim's last heartbeat", i.e. 4-6 s for the
  5 s threshold. Rescheduling adds ~2 s: up to one heartbeat for the assignment to reach a worker
  and up to one more for that worker to report the job RUNNING (RUNNING is only learned from
  heartbeats). Both are consequences of the 1 s pull-based heartbeat design.
* Every job finished in all 15 runs. The extra executions are exactly the killed attempts
  (executions = 24 + jobs lost); no job ran twice concurrently.

### Experiment B2: duplicate execution and fencing (frozen agent)

One agent is frozen with `SIGSTOP` (a hung or partitioned agent; its jobs keep running), then
resumed with `SIGCONT`. 12 jobs of 40 s, 5 repetitions. Source: `results/exp_b2.txt`.

| jobs on the frozen worker | frozen -> DEAD (s) | jobs running twice at once | zombie processes alive before heal | heal -> all zombies killed (s) |
|---|---|---|---|---|
| 4 | 4.80 (4.75..4.91) | 4 | 4 | 0.06 |

This is the at-least-once trade-off made visible: after the worker is declared dead its jobs run in
two places at once. After the heal the agent's stale heartbeat is rejected, it re-registers, and the
control plane tells it to kill the superseded attempts (0.06 s). The live attempts were unaffected.

### Experiment C: control-plane crash (SIGKILL) and WAL restart

40 jobs of 10 s on 9 workers; the control plane is `kill -9`'d with all 40 running, then restarted
with the same WAL. 5 repetitions. Source: `results/exp_c.txt`.

| WAL size | restart -> serving | restart -> all 9 workers ALIVE | jobs lost | jobs executed more than once | jobs with attempts > 1 |
|---|---|---|---|---|---|
| 12,865 bytes | 0.02 s | 0.19 s (0.10..0.36) | 0 / 5 runs | 0 / 5 runs | 0 / 5 runs |

Replay of a 12.9 KB log is negligible; recovery time here is dominated by workers' next heartbeat.
This says nothing about large logs (no snapshots; replay is O(history)). On the `kind` cluster
(real pods, PVC), killing the control-plane pod took 3.2 s to have all 9 workers re-registered and
re-assigned a few jobs that had been assigned but not yet delivered (DECISIONS D38/D39).

### WAL cost (single writer, `/tmp` on ext4 over a WSL2 virtual disk)

Source: `results/walbench.txt`. 3000 appends of one ~150-byte record.

| sync mode | appends/s | p50 | p99 | max |
|---|---|---|---|---|
| `every` (fdatasync each append; the default) | 255 | 3.8 ms | 6.3 ms | 18.5 ms |
| `interval` (10 ms background sync) | 690,000 | 1 us | 5 us | 108 us |
| `none` | 542,000 | 2 us | 5 us | 50 us |

With `every`, a batch of 10 records costs the same one fsync (276 frames/s, so ~2,760 records/s).
Every mutation (submit, assignment, completion, ...) takes the global lock for one fsync, so on this
disk the control plane tops out at a few hundred mutations per second: a job's lifecycle needs three
(submit, assign, finish). `interval` mode removes the cost but can lose the last ~10 ms of
*acknowledged* operations in a crash. These are properties of this virtual disk, not of GKE's.

## Deviations from DESIGN.md

* `SCHEDULED -> SUCCEEDED` is possible without a RUNNING state (D18); DESIGN.md was updated.
* `REGISTER` records are journaled and replay invalidates epochs (D12, D33).
* Idempotency keys (`request_id`) on submit (D23), channel keepalive/rebuild and a recovery grace
  period (D38), detector self-monitoring (D36): all added after bugs found while testing.
* A hand-rolled `/metrics` endpoint instead of prometheus-cpp (D37).
* Snapshots / log truncation were not implemented (stretch goal). Stretch items (Pub/Sub bridge,
  priorities/backfill, Raft) were not attempted.
* "Scheduling latency" in the simulator is called **queue wait time** (instant scheduler).



## Repository layout

```
proto/kestrel.proto        wire API            src/core      model, state, policies, scheduler
src/sim                    simulator           src/wal       write-ahead log
src/control                control plane       src/worker    worker agent
src/cli                    kestrelctl, loadgen, walbench      src/common  log, flags, histogram
tests/                     unit + integration + failure + restart + reconnect tests
deploy/k8s, deploy/kind    kustomize base + overlays          terraform/  GKE Autopilot (not applied)
Dockerfile, cloudbuild.yaml                                    scripts/    build, demos, experiments
results/                   raw experiment outputs and summaries
```
