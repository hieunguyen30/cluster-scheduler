#include "sim/workload.h"

#include <algorithm>

#include "sim/rng.h"

namespace kestrel::sim {

std::vector<JobClass> DefaultJobClasses() {
  // name, {cpu_millis, mem_mib}, min_dur, max_dur, weight
  return {
      {"small", {100, 128}, 1 * kSecond, 5 * kSecond, 40},
      {"medium", {500, 1024}, 2 * kSecond, 10 * kSecond, 25},
      {"large", {2000, 4096}, 5 * kSecond, 20 * kSecond, 10},
      {"cpu-heavy", {2000, 512}, 3 * kSecond, 12 * kSecond, 12},
      {"mem-heavy", {500, 8192}, 3 * kSecond, 12 * kSecond, 13},
  };
}

std::vector<Resources> MakeFleet(int num_workers) {
  static const Resources kTypes[] = {{2000, 8192}, {4000, 16384}, {8000, 32768}};
  std::vector<Resources> fleet;
  for (int i = 0; i < num_workers; ++i) fleet.push_back(kTypes[i % 3]);
  return fleet;
}

TimeUs MeanInterarrival(const WorkloadConfig& cfg) {
  double total_w = 0, mean_cpu = 0, mean_mem = 0, mean_dur = 0;
  for (const JobClass& c : cfg.classes) total_w += c.weight;
  for (const JobClass& c : cfg.classes) {
    double p = c.weight / total_w;
    double dur = (static_cast<double>(c.min_duration) + static_cast<double>(c.max_duration)) / 2;
    mean_cpu += p * static_cast<double>(c.request.cpu_millis) * dur;
    mean_mem += p * static_cast<double>(c.request.mem_mib) * dur;
    mean_dur += p * dur;
  }
  (void)mean_dur;
  double cap_cpu = 0, cap_mem = 0;
  for (const Resources& r : cfg.fleet) {
    cap_cpu += static_cast<double>(r.cpu_millis);
    cap_mem += static_cast<double>(r.mem_mib);
  }
  // Resource-time demanded by one job divided by the resource-time available per unit time
  // at the target load gives the arrival gap; the larger gap-demand wins (more constrained).
  double gap_cpu = mean_cpu / (cfg.target_load * cap_cpu);
  double gap_mem = mean_mem / (cfg.target_load * cap_mem);
  return static_cast<TimeUs>(std::max(gap_cpu, gap_mem));
}

std::vector<SimJob> GenerateWorkload(const WorkloadConfig& cfg) {
  Rng rng(cfg.seed);
  const TimeUs mean_gap = MeanInterarrival(cfg);
  std::vector<uint32_t> weights;
  for (const JobClass& c : cfg.classes) weights.push_back(c.weight);

  std::vector<SimJob> jobs;
  jobs.reserve(cfg.num_jobs);
  TimeUs t = 0;
  for (int i = 0; i < cfg.num_jobs; ++i) {
    const JobClass& c = cfg.classes[rng.Weighted(weights)];
    SimJob j;
    j.arrival = t;
    j.duration = static_cast<TimeUs>(rng.Uniform(c.min_duration, c.max_duration));
    j.spec.command = "sleep";
    j.spec.request = c.request;
    j.job_class = c.name;
    jobs.push_back(std::move(j));
    t += static_cast<TimeUs>(rng.Uniform(0, 2 * static_cast<uint64_t>(mean_gap)));
  }
  return jobs;
}

}  // namespace kestrel::sim
