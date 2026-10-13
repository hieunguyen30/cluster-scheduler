// The control plane: ClusterState + Scheduler behind ONE coarse mutex, a scheduler thread and a
// failure-detector thread (DESIGN.md section 2). gRPC handlers are thin wrappers over this class.
//
// Locking rule: every public method takes `mu_` for its whole body. Nothing blocks on the
// network while holding it; the only slow thing under the lock is the journal fsync, which is
// the price of acknowledging only durable state (documented scalability limit).
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "common/histogram.h"
#include "core/cluster_state.h"
#include "core/scheduler.h"

namespace kestrel {

struct ControlPlaneConfig {
  std::string policy = "best-fit";
  TimeUs suspect_after = 3 * kSecond;
  TimeUs dead_after = 5 * kSecond;
  TimeUs detector_interval = 250 * kMillisecond;
  TimeUs scheduler_tick = 500 * kMillisecond;  // safety net; real wakeups are event-driven
  int default_max_attempts = 3;
  // After WAL replay, workers get dead_after + recovery_grace to re-register before they can be
  // declared dead. A restarted control plane is unreachable for a while from the workers' side
  // (pod restart, black-holed connections, DNS), and killing live workers for that would
  // requeue jobs that are running fine. Cost: workers that died WITH the control plane are
  // noticed that much later.
  TimeUs recovery_grace = 0;
  std::function<TimeUs()> clock;  // defaults to wall-clock microseconds
};

struct ClusterStats {
  size_t pending_depth = 0;
  size_t jobs_by_state[7] = {};  // indexed by JobState
  size_t workers_alive = 0, workers_suspect = 0, workers_dead = 0;
  Resources capacity_alive;   // sum over ALIVE workers
  Resources allocated_alive;  // allocated on ALIVE workers
  Counters counters;
  uint64_t detector_stalls = 0;  // ticks that found the control plane itself had been stalled
};

class ControlPlane {
 public:
  // `journal` may be null (no persistence). Not owned.
  ControlPlane(ControlPlaneConfig cfg, Journal* journal = nullptr);
  ~ControlPlane();

  // Rebuild state from journaled records before Start(). All known workers become SUSPECT.
  void Recover(std::span<const Record> records);

  void Start();
  void Stop();

  // ---- client operations ----
  Result<JobId> SubmitJob(JobSpec spec);
  Result<Job> GetJob(JobId id, std::string* worker_name = nullptr);
  std::vector<Job> ListJobs(std::optional<JobState> filter, size_t limit);
  Status CancelJob(JobId id);
  std::vector<Worker> ListWorkers();

  // ---- worker operations ----
  Result<RegisterResult> Register(const std::string& name, Resources capacity,
                                  std::span<const RunningReport> running,
                                  std::span<const CompletedReport> completed);
  HeartbeatResult Heartbeat(WorkerId worker, Epoch epoch, std::span<const RunningReport> running,
                            std::span<const CompletedReport> completed);

  ClusterStats Snapshot();
  // Submit -> first assignment, in seconds, as observed by the real scheduler thread. This
  // includes time spent waiting for capacity; it is NOT pure decision latency.
  const Histogram& scheduling_latency() const { return sched_latency_; }
  // Run `fn` with the state locked. Test/diagnostic hook only.
  void WithState(const std::function<void(const ClusterState&)>& fn);

  const ControlPlaneConfig& config() const { return cfg_; }
  TimeUs Now() const { return cfg_.clock(); }

 private:
  void SchedulerLoop();
  void DetectorLoop();
  void WakeScheduler();  // requires mu_ held

  ControlPlaneConfig cfg_;
  std::mutex mu_;
  std::condition_variable sched_cv_;
  std::condition_variable detector_cv_;
  bool wake_ = false;
  bool stop_ = false;
  uint64_t detector_stalls_ = 0;  // guarded by mu_
  bool started_ = false;
  ClusterState state_;
  Scheduler scheduler_;
  Histogram sched_latency_{Histogram::LatencyBounds()};
  std::thread sched_thread_, detector_thread_;
};

}  // namespace kestrel
