# Kestrel

A small Borg/Kubernetes-inspired cluster scheduler in C++20. Clients submit jobs with CPU/memory
requests; a control plane places them on worker agents using a pluggable bin-packing policy
(round-robin, first-fit, best-fit); dead workers are detected by heartbeat and their jobs
rescheduled (at-least-once, with epoch/generation fencing); a write-ahead log lets the control plane
survive a crash. Ships as one container image with Kubernetes manifests, Terraform and a Cloud Build
pipeline for GKE Autopilot ([docs/GKE.md](docs/GKE.md)).


## Architecture

```
   kestrelctl / kestrel_loadgen  --gRPC-->  kestrel_control (one process, one mutex)
                                              ClusterState + FIFO queue + PlacementPolicy
                                              threads: gRPC handlers, scheduler, failure detector
                                              WAL file (CRC32C frames, fdatasync)  /metrics :9090
                                                 ^ Register / Heartbeat (replies carry assignments,
                                                 |   kill orders, acks)
                                         kestrel_worker x N  (runs each job as a subprocess)
```

* **Failure model:** SUSPECT after 3 s of silence (no new placements), DEAD after 5 s (jobs requeued
  at the front). Each registration gets a new epoch (fences zombie workers); each assignment a new
  generation (fences zombie results).
* **Restart:** replay the WAL, invalidate all epochs, workers re-register reporting what they run;
  matching jobs are adopted, unknown ones killed.
* **Simulator:** virtual-time discrete-event simulation that drives the *same* scheduler core, with a
  seeded workload generator.

## Build and run (WSL2 Ubuntu 24.04)

```bash
sudo apt-get install -y build-essential clang cmake ninja-build pkg-config \
     libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc python3
scripts/build.sh plain|asan|tsan   # build into ~/kestrel-build/<mode> and run ctest
scripts/demo_local.sh              # control plane + 2 workers + CLI
scripts/demo_restart.sh            # kill -9 the control plane mid-run, restart from the WAL
scripts/kind_up.sh && scripts/kind_chaos.sh   # local Kubernetes, kill worker and control pods
kestrel_sim --policy all --seeds 50 --workers 9 --jobs 2000 --load 1.0 --csv out.csv
```

**117 tests** pass in plain, ASan+UBSan and TSan builds (unit, WAL truncation sweep, real-gRPC
integration, failure, restart and reconnect tests). TSan runs with `ignore_noninstrumented_modules`
and three narrow suppressions for the distro's uninstrumented gRPC ([tests/tsan.supp](tests/tsan.supp)):
no race was found in Kestrel's own code, but TSan cannot see inside gRPC.

## Results

Raw outputs and setup records are in [results/](results/); seeds are fixed. Real-system experiments
are local processes over loopback, not a cloud.

* **Placement policy (simulator, 50 seeds x 2000 jobs, 9 workers, paired by seed).** At load >= 0.85,
  best-fit beats first-fit, which beats round-robin, with 95% CIs excluding zero, but the effect is
  **small**: about +1 point of CPU utilisation, 0.1-0.9% shorter makespan, 5-11% lower p99 queue wait
  (load 1.0: makespan 316 / 311 / 309 s for round-robin / first-fit / best-fit). At load 0.6 the
  differences are noise. FIFO head-of-line blocking dominates: the queue head is blocked while
  aggregate free capacity would fit it for 80-95% of busy time, for every policy.
* **Same comparison on the real system (8 seeds, WAL on).** Round-robin is clearly worst; best-fit
  vs first-fit is **not distinguishable** (per-seed differences straddle zero).
* **Worker failure (kill K of 9 nodes, 5 runs each).** Detected as DEAD 4.5-6 s after the kill
  (5 s threshold); lost jobs running again ~2 s later (heartbeat-driven dispatch); all jobs finished
  in 15/15 runs, no concurrent duplicates.
* **Frozen agent (SIGSTOP/SIGCONT).** 4 jobs ran twice at once until the agent was resumed; fencing
  killed the stale copies 0.06 s after the heal.
* **Control-plane `kill -9` (5 runs, 40 running jobs).** 0 jobs lost, 0 executed twice; all workers
  back in 0.19 s (a 13 KB log; says nothing about large logs).
* **WAL cost.** fsync on every append: ~255 appends/s (p50 3.8 ms) on this disk vs ~500k/s without.
  This bounds mutation throughput under the global lock.

## Layout

```
proto/  src/{core,sim,wal,control,worker,cli,common}  tests/  scripts/  results/
deploy/{k8s,kind}  terraform/  Dockerfile  cloudbuild.yaml  docs/GKE.md
```
