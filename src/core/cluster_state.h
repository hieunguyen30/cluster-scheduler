// In-memory cluster state: jobs, workers, FIFO pending queue and every legal transition.
//
// Not thread-safe by design. The control plane wraps it in one coarse mutex; the simulator
// is single-threaded. Keeping the lock outside keeps this logic testable in isolation.
#pragma once

#include <deque>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/model.h"

namespace kestrel {

// A job a worker reports as running (heartbeat / register).
struct RunningReport {
  JobId job = 0;
  uint64_t generation = 0;
};

// A job a worker reports as finished (re-sent until acknowledged).
struct CompletedReport {
  JobId job = 0;
  uint64_t generation = 0;
  int32_t exit_code = 0;
};

// What the control plane tells a worker to run.
struct AssignmentOrder {
  JobId job = 0;
  uint64_t generation = 0;
  JobSpec spec;
};

struct RegisterResult {
  WorkerId worker = 0;
  Epoch epoch = 0;
  std::vector<JobId> kill;   // reported jobs this worker must not keep running
  std::vector<JobId> acked;  // completed reports processed
};

struct HeartbeatResult {
  bool reregister = false;  // stale epoch / unknown / declared dead: worker must Register
  std::vector<AssignmentOrder> assignments;
  std::vector<JobId> kill;
  std::vector<JobId> acked;  // completed reports the control plane has processed
};

struct Counters {
  uint64_t jobs_submitted = 0;
  uint64_t jobs_lost = 0;         // LOST transitions (each one is a lost attempt)
  uint64_t jobs_rescheduled = 0;  // LOST -> PENDING
  uint64_t stale_reports_ignored = 0;
  uint64_t replay_skipped = 0;    // records that did not apply cleanly during replay
};

class ClusterState {
 public:
  explicit ClusterState(Journal* journal = nullptr) : journal_(journal) {}
  void SetJournal(Journal* j) { journal_ = j; }

  // ---- jobs ------------------------------------------------------------------------------
  Result<JobId> Submit(JobSpec spec, TimeUs now);
  // PENDING -> SCHEDULED on `worker`. The caller (scheduler) has already checked fit.
  Status Assign(JobId job, WorkerId worker, TimeUs now);
  // Cancel from any non-terminal state.
  Status Cancel(JobId job, TimeUs now);
  // Worker confirms it started the job. Not journaled: RUNNING is re-derived on reconcile.
  Status ReportStarted(JobId job, uint64_t generation, WorkerId worker, TimeUs now);
  // Worker reports completion. Reports from a superseded attempt are ignored (ok status,
  // counted in stale_reports_ignored) -- that is the assignment_generation fence.
  Status ReportFinished(JobId job, uint64_t generation, WorkerId worker, int32_t exit_code,
                        TimeUs now);

  // ---- workers ---------------------------------------------------------------------------
  // Register (or re-register) the worker called `name`. A new registration always gets a new
  // epoch. `running` is what the agent currently runs: jobs that match our records are adopted
  // (kept), others go in `kill`; jobs we think are on the worker but it does not report are LOST.
  // `completed` lets a worker deliver results it still holds from before a re-register.
  Result<RegisterResult> RegisterWorker(const std::string& name, Resources capacity,
                                        std::span<const RunningReport> running,
                                        std::span<const CompletedReport> completed, TimeUs now);
  HeartbeatResult Heartbeat(WorkerId worker, Epoch epoch, std::span<const RunningReport> running,
                            std::span<const CompletedReport> completed, TimeUs now);
  // Failure detector tick. Moves ALIVE->SUSPECT after `suspect_after` of silence and
  // ->DEAD after `dead_after`, requeueing the jobs of newly dead workers. Returns new DEADs.
  std::vector<WorkerId> CheckWorkers(TimeUs now, TimeUs suspect_after, TimeUs dead_after);
  Status MarkWorkerDead(WorkerId worker, TimeUs now);

  // ---- replay (phase 4) ------------------------------------------------------------------
  // Apply a previously journaled record without journaling it again.
  void Replay(const Record& r);
  // After replay every known worker is "unknown": SUSPECT with heartbeat = now, so the failure
  // detector declares it DEAD unless it re-registers within the dead timeout.
  void FinishReplay(TimeUs now);

  // ---- queries ---------------------------------------------------------------------------
  const Job* FindJob(JobId id) const;
  const Worker* FindWorker(WorkerId id) const;
  const Worker* FindWorkerByName(const std::string& name) const;
  std::span<const Worker> Workers() const { return workers_; }
  const std::deque<JobId>& Pending() const { return pending_; }
  std::optional<JobId> PendingHead() const;
  size_t JobCount() const { return jobs_.size(); }
  std::vector<Job> ListJobs() const;  // ordered by id
  size_t CountJobs(JobState s) const;
  const Counters& counters() const { return counters_; }

  // Consistency check used by tests and the simulator. Empty string means consistent.
  std::string CheckInvariants() const;

 private:
  Job* MutableJob(JobId id);
  Worker* MutableWorker(WorkerId id);
  // Journal-then-apply for a batch of records.
  Status Commit(const std::vector<Record>& records);
  void Apply(const Record& r);
  void Release(Job& job);
  std::vector<Record> LostRecords(const Worker& w, TimeUs now) const;

  Journal* journal_;
  std::unordered_map<JobId, Job> jobs_;
  std::deque<JobId> pending_;
  std::vector<Worker> workers_;  // never shrinks; vector index order == creation order
  std::unordered_map<WorkerId, size_t> worker_index_;
  std::unordered_map<std::string, WorkerId> worker_by_name_;
  JobId next_job_id_ = 1;
  WorkerId next_worker_id_ = 1;
  Epoch next_epoch_ = 1;
  Counters counters_;
};

}  // namespace kestrel
