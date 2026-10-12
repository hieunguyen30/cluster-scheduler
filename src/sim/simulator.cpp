#include "sim/simulator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <queue>

#include "core/cluster_state.h"
#include "core/scheduler.h"

namespace kestrel::sim {

namespace {

enum class EventType { ARRIVAL, COMPLETION };

struct Event {
  TimeUs time;
  uint64_t seq;
  EventType type;
  size_t job_index;     // ARRIVAL: index into the workload
  JobId job = 0;        // COMPLETION
  uint64_t generation = 0;
  WorkerId worker = 0;
};

struct EventLater {
  bool operator()(const Event& a, const Event& b) const {
    if (a.time != b.time) return a.time > b.time;
    return a.seq > b.seq;
  }
};

[[noreturn]] void Die(const std::string& msg) {
  std::fprintf(stderr, "simulator fatal: %s\n", msg.c_str());
  std::abort();
}

}  // namespace

TimeUs Percentile(std::vector<TimeUs> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  size_t rank = static_cast<size_t>(std::ceil(p * static_cast<double>(v.size())));
  rank = std::min(std::max<size_t>(rank, 1), v.size());
  return v[rank - 1];
}

SimResult RunSimulation(std::string_view policy_name, uint64_t seed,
                        const std::vector<Resources>& fleet, const std::vector<SimJob>& jobs,
                        const SimOptions& opts) {
  auto policy = MakePolicy(policy_name);
  if (!policy) Die("unknown policy");
  SimResult res;
  res.policy = policy->Name();
  res.seed = seed;
  res.jobs = jobs.size();
  if (jobs.empty()) return res;

  Scheduler scheduler(std::move(policy));
  ClusterState state;
  Resources total_capacity;
  for (size_t i = 0; i < fleet.size(); ++i) {
    auto r = state.RegisterWorker("sim-worker-" + std::to_string(i), fleet[i], {}, {}, 0);
    if (!r.ok()) Die(r.status().message());
    total_capacity += fleet[i];
  }

  std::priority_queue<Event, std::vector<Event>, EventLater> events;
  uint64_t seq = 0;
  for (size_t i = 0; i < jobs.size(); ++i)
    events.push(Event{jobs[i].arrival, seq++, EventType::ARRIVAL, i});

  std::vector<TimeUs> durations;  // indexed by JobId - 1 (ids are assigned 1,2,3,... in order)
  durations.reserve(jobs.size());

  const TimeUs t0 = events.top().time;
  TimeUs prev = t0;
  TimeUs last_completion = t0;

  // Integrals: sum over time of allocated resource (units * us).
  double cpu_area = 0, mem_area = 0, cpu_area_busy = 0, mem_area_busy = 0;
  TimeUs busy_time = 0, frag_time = 0;

  while (!events.empty()) {
    const TimeUs now = events.top().time;

    // 1. Integrate the (constant) state over [prev, now).
    const TimeUs dt = now - prev;
    if (dt > 0) {
      Resources alloc, free_alive;
      for (const Worker& w : state.Workers()) {
        alloc += w.allocated;
        if (w.status == WorkerStatus::ALIVE) free_alive += w.Free();
      }
      const double dcpu = static_cast<double>(alloc.cpu_millis) * static_cast<double>(dt);
      const double dmem = static_cast<double>(alloc.mem_mib) * static_cast<double>(dt);
      cpu_area += dcpu;
      mem_area += dmem;
      if (auto head = state.PendingHead()) {
        busy_time += dt;
        cpu_area_busy += dcpu;
        mem_area_busy += dmem;
        // After a scheduling pass the head fits on no single worker. If it would fit in the
        // aggregate free capacity, the blockage is fragmentation, not lack of capacity.
        if (state.FindJob(*head)->spec.request.FitsIn(free_alive)) frag_time += dt;
      }
    }
    prev = now;

    // 2. Apply every event at this timestamp.
    while (!events.empty() && events.top().time == now) {
      Event e = events.top();
      events.pop();
      if (e.type == EventType::ARRIVAL) {
        auto id = state.Submit(jobs[e.job_index].spec, now);
        if (!id.ok()) Die(id.status().message());
        if (id.value() != durations.size() + 1) Die("job id sequence assumption violated");
        durations.push_back(jobs[e.job_index].duration);
      } else {
        Status s = state.ReportFinished(e.job, e.generation, e.worker, 0, now);
        if (!s.ok()) Die(s.message());
        last_completion = now;
      }
    }

    // 3. One scheduling pass; placed jobs start immediately (no startup delay modelled).
    for (JobId id : scheduler.RunPass(state, now)) {
      const Job* j = state.FindJob(id);
      state.ReportStarted(id, j->generation, j->assigned_worker, now);
      events.push(Event{now + durations[id - 1], seq++, EventType::COMPLETION, 0, id,
                        j->generation, j->assigned_worker});
    }
    res.max_queue_depth = std::max(res.max_queue_depth, state.Pending().size());

    if (opts.check_invariants) {
      std::string err = state.CheckInvariants();
      if (!err.empty()) Die("invariant violated at t=" + std::to_string(now) + ": " + err);
    }
  }

  // Collect per-job results.
  std::vector<TimeUs> waits;
  double wait_sum = 0;
  for (const Job& j : state.ListJobs()) {
    if (j.state == JobState::SUCCEEDED) ++res.jobs_succeeded;
    if (j.first_scheduled != kUnsetTime) {
      TimeUs w = j.first_scheduled - j.submitted;
      waits.push_back(w);
      wait_sum += static_cast<double>(w);
    }
  }
  res.makespan_us = last_completion - t0;
  const double span = static_cast<double>(res.makespan_us);
  if (span > 0) {
    res.cpu_util_makespan = cpu_area / (span * static_cast<double>(total_capacity.cpu_millis));
    res.mem_util_makespan = mem_area / (span * static_cast<double>(total_capacity.mem_mib));
    res.busy_fraction = static_cast<double>(busy_time) / span;
    res.frag_fraction_makespan = static_cast<double>(frag_time) / span;
  }
  if (busy_time > 0) {
    const double b = static_cast<double>(busy_time);
    res.cpu_util_busy = cpu_area_busy / (b * static_cast<double>(total_capacity.cpu_millis));
    res.mem_util_busy = mem_area_busy / (b * static_cast<double>(total_capacity.mem_mib));
    res.frag_fraction_busy = static_cast<double>(frag_time) / b;
  }
  res.wait_p50_us = Percentile(waits, 0.50);
  res.wait_p99_us = Percentile(waits, 0.99);
  if (!waits.empty()) res.wait_mean_us = wait_sum / static_cast<double>(waits.size());
  return res;
}

}  // namespace kestrel::sim
