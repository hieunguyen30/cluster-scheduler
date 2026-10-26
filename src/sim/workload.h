// Synthetic workload and worker-fleet generation for the simulator and benchmarks.
#pragma once

#include <string>
#include <vector>

#include "core/model.h"

namespace kestrel::sim {

struct JobClass {
  std::string name;
  Resources request;
  TimeUs min_duration;
  TimeUs max_duration;
  uint32_t weight;
};

struct SimJob {
  TimeUs arrival = 0;
  TimeUs duration = 0;
  JobSpec spec;
  std::string job_class;
};

// The documented default mix (see README): small, medium, large, plus
// CPU-heavy and memory-heavy jobs so that fragmentation can appear.
std::vector<JobClass> DefaultJobClasses();

// Heterogeneous fleet: cycles through 2c/8GiB, 4c/16GiB, 8c/32GiB workers.
std::vector<Resources> MakeFleet(int num_workers);

struct WorkloadConfig {
  uint64_t seed = 1;
  int num_jobs = 2000;
  double target_load = 0.85;  // offered load as a fraction of the more constrained resource
  std::vector<JobClass> classes = DefaultJobClasses();
  std::vector<Resources> fleet = MakeFleet(10);
};

// Mean inter-arrival time that offers `target_load` of the fleet's CPU or memory (whichever is
// more constrained), assuming the class mix and mean durations. Pure arithmetic on the config.
TimeUs MeanInterarrival(const WorkloadConfig& cfg);

// Deterministic for a given config (same seed => same stream on the same toolchain).
// Inter-arrival gaps are uniform in [0, 2*mean] (integer-only; no libm).
std::vector<SimJob> GenerateWorkload(const WorkloadConfig& cfg);

}  // namespace kestrel::sim
