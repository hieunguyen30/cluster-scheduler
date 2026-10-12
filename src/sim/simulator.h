// Discrete-event simulator: virtual integer-microsecond clock, simulated workers in-process,
// driving the SAME ClusterState / Scheduler / policies the real control plane uses.
//
// Events are ordered by (time, sequence) so runs are fully deterministic. All events sharing a
// timestamp are applied before the scheduler runs once.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "sim/workload.h"

namespace kestrel::sim {

struct SimResult {
  std::string policy;
  uint64_t seed = 0;
  size_t jobs = 0;
  size_t jobs_succeeded = 0;
  TimeUs makespan_us = 0;  // first arrival -> last completion

  // Time-averaged utilization = allocated / capacity, integrated over time.
  double cpu_util_makespan = 0;  // over the whole makespan (includes the ramp-up/drain tails)
  double mem_util_makespan = 0;
  double cpu_util_busy = 0;      // only over time when the pending queue is non-empty
  double mem_util_busy = 0;
  double busy_fraction = 0;      // fraction of the makespan with a non-empty queue

  // Fragmentation: head-of-queue job is blocked although aggregate free CPU and memory across
  // alive workers would fit it (no single worker has room). Head job only: jobs behind a
  // blocked head are not counted, so this under-states stranded demand.
  double frag_fraction_makespan = 0;
  double frag_fraction_busy = 0;

  // "Queue wait time": submit -> first assignment. NOT end-to-end scheduling latency (the
  // simulated scheduler is instantaneous in virtual time, so there is no decision latency).
  TimeUs wait_p50_us = 0;
  TimeUs wait_p99_us = 0;
  double wait_mean_us = 0;
  size_t max_queue_depth = 0;
};

struct SimOptions {
  bool check_invariants = false;  // run ClusterState::CheckInvariants after every event batch
};

// Run `jobs` on a fleet of workers (all registered at t=0) under the named policy.
// Aborts the process with a message if an invariant check fails or the policy is unknown.
SimResult RunSimulation(std::string_view policy, uint64_t seed, const std::vector<Resources>& fleet,
                        const std::vector<SimJob>& jobs, const SimOptions& opts = {});

// Nearest-rank percentile (p in (0,1]) of an unsorted vector.
TimeUs Percentile(std::vector<TimeUs> values, double p);

}  // namespace kestrel::sim
