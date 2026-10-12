#include "core/cluster_state.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace kestrel {

namespace {
bool IsActive(const Job& j) {
  return j.state == JobState::SCHEDULED || j.state == JobState::RUNNING;
}
}  // namespace

// ---- lookup helpers ------------------------------------------------------------------------

const Job* ClusterState::FindJob(JobId id) const {
  auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}
Job* ClusterState::MutableJob(JobId id) {
  auto it = jobs_.find(id);
  return it == jobs_.end() ? nullptr : &it->second;
}
const Worker* ClusterState::FindWorker(WorkerId id) const {
  auto it = worker_index_.find(id);
  return it == worker_index_.end() ? nullptr : &workers_[it->second];
}
Worker* ClusterState::MutableWorker(WorkerId id) {
  auto it = worker_index_.find(id);
  return it == worker_index_.end() ? nullptr : &workers_[it->second];
}
const Worker* ClusterState::FindWorkerByName(const std::string& name) const {
  auto it = worker_by_name_.find(name);
  return it == worker_by_name_.end() ? nullptr : FindWorker(it->second);
}
std::optional<JobId> ClusterState::PendingHead() const {
  if (pending_.empty()) return std::nullopt;
  return pending_.front();
}
std::vector<Job> ClusterState::ListJobs() const {
  std::vector<Job> out;
  out.reserve(jobs_.size());
  for (const auto& kv : jobs_) out.push_back(kv.second);
  std::sort(out.begin(), out.end(), [](const Job& a, const Job& b) { return a.id < b.id; });
  return out;
}
size_t ClusterState::CountJobs(JobState s) const {
  size_t n = 0;
  for (const auto& kv : jobs_) n += kv.second.state == s;
  return n;
}

// ---- journal + apply -----------------------------------------------------------------------

Status ClusterState::Commit(const std::vector<Record>& records) {
  if (records.empty()) return Status::Ok();
  // Write-ahead: the records are durable before any in-memory state changes. If the process
  // dies after Append but before Apply, replay re-creates exactly the state we would have had.
  if (journal_ != nullptr) {
    Status s = journal_->Append(records);
    if (!s.ok()) return s;
  }
  for (const Record& r : records) Apply(r);
  return Status::Ok();
}

void ClusterState::Release(Job& job) {
  if (Worker* w = MutableWorker(job.assigned_worker)) {
    w->allocated -= job.spec.request;
    w->jobs.erase(job.id);
  }
}

// Apply is deterministic in (state, record) and defensive: a record that does not fit the
// current state is skipped and counted instead of corrupting accounting.
void ClusterState::Apply(const Record& r) {
  switch (r.type) {
    case RecordType::SUBMIT: {
      if (jobs_.count(r.job)) {
        ++counters_.replay_skipped;
        return;
      }
      if (!r.spec.request_id.empty()) job_by_request_id_.emplace(r.spec.request_id, r.job);
      Job j;
      j.id = r.job;
      j.spec = r.spec;
      j.state = JobState::PENDING;
      j.submitted = r.time;
      jobs_.emplace(j.id, std::move(j));
      pending_.push_back(r.job);
      next_job_id_ = std::max(next_job_id_, r.job + 1);
      ++counters_.jobs_submitted;
      return;
    }
    case RecordType::ASSIGN: {
      Job* j = MutableJob(r.job);
      Worker* w = MutableWorker(r.worker);
      if (j == nullptr || w == nullptr || j->state != JobState::PENDING) {
        ++counters_.replay_skipped;
        return;
      }
      auto it = std::find(pending_.begin(), pending_.end(), r.job);
      if (it != pending_.end()) pending_.erase(it);
      j->state = JobState::SCHEDULED;
      j->assigned_worker = r.worker;
      j->generation = r.generation;
      j->attempts += 1;
      j->scheduled = r.time;
      if (j->first_scheduled == kUnsetTime) j->first_scheduled = r.time;
      w->allocated += j->spec.request;
      w->jobs.insert(j->id);
      return;
    }
    case RecordType::FINISH: {
      Job* j = MutableJob(r.job);
      if (j == nullptr || !IsActive(*j)) {
        ++counters_.replay_skipped;
        return;
      }
      Release(*j);
      j->state = r.exit_code == 0 ? JobState::SUCCEEDED : JobState::FAILED;
      j->exit_code = r.exit_code;
      j->finished = r.time;
      return;
    }
    case RecordType::LOST: {
      Job* j = MutableJob(r.job);
      if (j == nullptr || !IsActive(*j)) {
        ++counters_.replay_skipped;
        return;
      }
      Release(*j);
      j->state = JobState::LOST;
      ++counters_.jobs_lost;
      if (j->attempts < j->spec.max_attempts) {
        // Requeue at the FRONT: it has already waited, and LostRecords are emitted in
        // descending id order so the final queue order is ascending by id.
        j->state = JobState::PENDING;
        j->assigned_worker = 0;
        pending_.push_front(j->id);
        ++counters_.jobs_rescheduled;
      } else {
        j->state = JobState::FAILED;
        j->finished = r.time;
      }
      return;
    }
    case RecordType::CANCEL: {
      Job* j = MutableJob(r.job);
      if (j == nullptr || IsTerminal(j->state)) {
        ++counters_.replay_skipped;
        return;
      }
      if (j->state == JobState::PENDING) {
        auto it = std::find(pending_.begin(), pending_.end(), r.job);
        if (it != pending_.end()) pending_.erase(it);
      } else {
        Release(*j);
      }
      j->state = JobState::CANCELLED;
      j->finished = r.time;
      return;
    }
    case RecordType::REGISTER: {
      auto by_name = worker_by_name_.find(r.name);
      Worker* w = nullptr;
      if (by_name != worker_by_name_.end()) {
        w = MutableWorker(by_name->second);
      } else {
        Worker nw;
        nw.id = r.worker;
        nw.name = r.name;
        worker_index_[nw.id] = workers_.size();
        worker_by_name_[nw.name] = nw.id;
        workers_.push_back(std::move(nw));
        w = &workers_.back();
      }
      w->epoch = r.epoch;
      w->capacity = r.capacity;
      w->status = WorkerStatus::ALIVE;
      w->last_heartbeat = r.time;
      next_worker_id_ = std::max(next_worker_id_, r.worker + 1);
      next_epoch_ = std::max(next_epoch_, r.epoch + 1);
      return;
    }
  }
}

void ClusterState::Replay(const Record& r) { Apply(r); }

void ClusterState::FinishReplay(TimeUs now) {
  for (Worker& w : workers_) {
    w.status = WorkerStatus::SUSPECT;
    w.last_heartbeat = now;
  }
}

// ---- jobs ----------------------------------------------------------------------------------

Result<JobId> ClusterState::Submit(JobSpec spec, TimeUs now) {
  if (spec.request.cpu_millis <= 0 || spec.request.mem_mib <= 0)
    return Status::Error("job request must be positive");
  if (spec.max_attempts < 1) return Status::Error("max_attempts must be >= 1");
  if (!spec.request_id.empty()) {
    auto it = job_by_request_id_.find(spec.request_id);
    if (it != job_by_request_id_.end()) return it->second;  // idempotent retry
  }
  Record r;
  r.type = RecordType::SUBMIT;
  r.time = now;
  r.job = next_job_id_;
  r.spec = std::move(spec);
  Status s = Commit({r});
  if (!s.ok()) return s;
  return r.job;
}

Status ClusterState::Assign(JobId job, WorkerId worker, TimeUs now) {
  const Job* j = FindJob(job);
  const Worker* w = FindWorker(worker);
  if (j == nullptr) return Status::Error("no such job");
  if (w == nullptr) return Status::Error("no such worker");
  if (j->state != JobState::PENDING) return Status::Error("job is not pending");
  if (w->status != WorkerStatus::ALIVE) return Status::Error("worker is not alive");
  if (!j->spec.request.FitsIn(w->Free())) return Status::Error("job does not fit on worker");
  Record r;
  r.type = RecordType::ASSIGN;
  r.time = now;
  r.job = job;
  r.worker = worker;
  r.generation = j->generation + 1;
  r.epoch = w->epoch;
  return Commit({r});
}

Status ClusterState::Cancel(JobId job, TimeUs now) {
  const Job* j = FindJob(job);
  if (j == nullptr) return Status::Error("no such job");
  if (IsTerminal(j->state)) return Status::Error("job already terminal");
  Record r;
  r.type = RecordType::CANCEL;
  r.time = now;
  r.job = job;
  return Commit({r});
}

Status ClusterState::ReportStarted(JobId job, uint64_t generation, WorkerId worker, TimeUs now) {
  Job* j = MutableJob(job);
  if (j == nullptr) return Status::Error("no such job");
  if (!IsActive(*j) || j->assigned_worker != worker || j->generation != generation) {
    ++counters_.stale_reports_ignored;
    return Status::Ok();
  }
  if (j->state == JobState::SCHEDULED) {
    j->state = JobState::RUNNING;
    j->started = now;
  }
  return Status::Ok();
}

Status ClusterState::ReportFinished(JobId job, uint64_t generation, WorkerId worker,
                                    int32_t exit_code, TimeUs now) {
  const Job* j = FindJob(job);
  if (j == nullptr) return Status::Error("no such job");
  if (!IsActive(*j) || j->assigned_worker != worker || j->generation != generation) {
    ++counters_.stale_reports_ignored;  // superseded attempt or already resolved
    return Status::Ok();
  }
  Record r;
  r.type = RecordType::FINISH;
  r.time = now;
  r.job = job;
  r.worker = worker;
  r.generation = generation;
  r.exit_code = exit_code;
  return Commit({r});
}

// ---- workers -------------------------------------------------------------------------------

std::vector<Record> ClusterState::LostRecords(const Worker& w, TimeUs now) const {
  std::vector<Record> out;
  for (auto it = w.jobs.rbegin(); it != w.jobs.rend(); ++it) {
    const Job* j = FindJob(*it);
    Record r;
    r.type = RecordType::LOST;
    r.time = now;
    r.job = *it;
    r.worker = w.id;
    r.generation = j ? j->generation : 0;
    out.push_back(std::move(r));
  }
  return out;
}

Result<RegisterResult> ClusterState::RegisterWorker(const std::string& name, Resources capacity,
                                                    std::span<const RunningReport> running,
                                                    std::span<const CompletedReport> completed,
                                                    TimeUs now) {
  if (name.empty()) return Status::Error("worker name required");
  if (capacity.cpu_millis <= 0 || capacity.mem_mib <= 0)
    return Status::Error("capacity must be positive");

  const Worker* existing = FindWorkerByName(name);
  RegisterResult res;
  res.worker = existing ? existing->id : next_worker_id_;
  res.epoch = next_epoch_;

  std::vector<Record> records;
  Record reg;
  reg.type = RecordType::REGISTER;
  reg.time = now;
  reg.worker = res.worker;
  reg.epoch = res.epoch;
  reg.name = name;
  reg.capacity = capacity;
  records.push_back(reg);

  std::set<JobId> resolved;  // jobs that are finished or adopted: must not be marked LOST
  std::vector<RunningReport> adopted;
  if (existing != nullptr) {
    // Completed results first: valid ones are applied, so those jobs are not re-run.
    for (const CompletedReport& c : completed) {
      res.acked.push_back(c.job);
      const Job* j = FindJob(c.job);
      if (j != nullptr && IsActive(*j) && j->assigned_worker == existing->id &&
          j->generation == c.generation && !resolved.count(c.job)) {
        Record f;
        f.type = RecordType::FINISH;
        f.time = now;
        f.job = c.job;
        f.worker = existing->id;
        f.generation = c.generation;
        f.exit_code = c.exit_code;
        records.push_back(f);
        resolved.insert(c.job);
      } else {
        ++counters_.stale_reports_ignored;
      }
    }
    for (const RunningReport& rr : running) {
      const Job* j = FindJob(rr.job);
      if (j != nullptr && IsActive(*j) && j->assigned_worker == existing->id &&
          j->generation == rr.generation && !resolved.count(rr.job)) {
        adopted.push_back(rr);
        resolved.insert(rr.job);
      } else {
        res.kill.push_back(rr.job);
      }
    }
    // Anything we think this worker holds but it did not report is gone (it restarted).
    // Note: includes SCHEDULED jobs whose assignment the worker never received; those are
    // simply re-placed, which is safe because the worker does not have them.
    std::vector<Record> lost = LostRecords(*existing, now);
    for (Record& l : lost)
      if (!resolved.count(l.job)) records.push_back(std::move(l));
  } else {
    for (const RunningReport& rr : running) res.kill.push_back(rr.job);  // we know nothing of them
    for (const CompletedReport& c : completed) res.acked.push_back(c.job);
  }

  Status s = Commit(records);
  if (!s.ok()) return s;
  for (const RunningReport& rr : adopted) ReportStarted(rr.job, rr.generation, res.worker, now);
  return res;
}

HeartbeatResult ClusterState::Heartbeat(WorkerId worker, Epoch epoch,
                                        std::span<const RunningReport> running,
                                        std::span<const CompletedReport> completed, TimeUs now) {
  HeartbeatResult res;
  Worker* w = MutableWorker(worker);
  // Fencing: a heartbeat is honoured only from the current incarnation of a worker that has
  // not been declared dead. Anything else must re-register and have its jobs reconciled.
  if (w == nullptr || w->epoch != epoch || w->status == WorkerStatus::DEAD) {
    res.reregister = true;
    return res;
  }
  w->last_heartbeat = now;
  w->status = WorkerStatus::ALIVE;

  std::vector<Record> records;
  std::set<JobId> resolved;
  for (const CompletedReport& c : completed) {
    res.acked.push_back(c.job);
    const Job* j = FindJob(c.job);
    if (j != nullptr && IsActive(*j) && j->assigned_worker == worker &&
        j->generation == c.generation && !resolved.count(c.job)) {
      Record f;
      f.type = RecordType::FINISH;
      f.time = now;
      f.job = c.job;
      f.worker = worker;
      f.generation = c.generation;
      f.exit_code = c.exit_code;
      records.push_back(f);
      resolved.insert(c.job);
    } else {
      ++counters_.stale_reports_ignored;
    }
  }

  std::set<JobId> reported;
  for (const RunningReport& rr : running) {
    const Job* j = FindJob(rr.job);
    if (j != nullptr && IsActive(*j) && j->assigned_worker == worker &&
        j->generation == rr.generation) {
      reported.insert(rr.job);
    } else {
      res.kill.push_back(rr.job);
    }
  }

  // A job we saw RUNNING that is neither reported running nor completed has vanished from the
  // worker (its process was lost without a result). Treat it as LOST.
  std::vector<Record> lost;
  for (JobId id : w->jobs) {
    const Job* j = FindJob(id);
    if (j != nullptr && j->state == JobState::RUNNING && !reported.count(id) &&
        !resolved.count(id)) {
      Record l;
      l.type = RecordType::LOST;
      l.time = now;
      l.job = id;
      l.worker = worker;
      l.generation = j->generation;
      lost.push_back(l);
    }
  }
  std::reverse(lost.begin(), lost.end());  // descending ids -> ascending queue order
  for (Record& l : lost) records.push_back(std::move(l));

  Status s = Commit(records);
  if (!s.ok()) {
    // Could not make the transitions durable: do not ack, the worker resends next beat.
    res.acked.clear();
    return res;
  }
  for (const RunningReport& rr : running)
    if (reported.count(rr.job)) ReportStarted(rr.job, rr.generation, worker, now);

  // Deliver assignments the worker has not started yet. Re-sent every beat until it reports
  // them, so a lost response only costs one heartbeat interval.
  for (JobId id : w->jobs) {
    const Job* j = FindJob(id);
    if (j != nullptr && j->state == JobState::SCHEDULED && !reported.count(id)) {
      AssignmentOrder o;
      o.job = id;
      o.generation = j->generation;
      o.spec = j->spec;
      res.assignments.push_back(std::move(o));
    }
  }
  return res;
}

Status ClusterState::MarkWorkerDead(WorkerId worker, TimeUs now) {
  Worker* w = MutableWorker(worker);
  if (w == nullptr) return Status::Error("no such worker");
  if (w->status == WorkerStatus::DEAD) return Status::Ok();
  Status s = Commit(LostRecords(*w, now));
  if (!s.ok()) return s;
  w->status = WorkerStatus::DEAD;
  return Status::Ok();
}

std::vector<WorkerId> ClusterState::CheckWorkers(TimeUs now, TimeUs suspect_after,
                                                 TimeUs dead_after) {
  std::vector<WorkerId> dead;
  // Index loop: MarkWorkerDead may not reallocate workers_, but keep it robust anyway.
  for (size_t i = 0; i < workers_.size(); ++i) {
    Worker& w = workers_[i];
    if (w.status == WorkerStatus::DEAD) continue;
    TimeUs silent = now - w.last_heartbeat;
    if (silent >= dead_after) {
      if (MarkWorkerDead(w.id, now).ok()) dead.push_back(w.id);
    } else if (silent >= suspect_after && w.status == WorkerStatus::ALIVE) {
      w.status = WorkerStatus::SUSPECT;
    }
  }
  return dead;
}

// ---- invariants ----------------------------------------------------------------------------

std::string ClusterState::CheckInvariants() const {
  std::ostringstream err;
  std::set<JobId> in_queue;
  for (JobId id : pending_) {
    if (!in_queue.insert(id).second) err << "job " << id << " queued twice; ";
    const Job* j = FindJob(id);
    if (j == nullptr || j->state != JobState::PENDING)
      err << "queued job " << id << " is not PENDING; ";
  }
  std::unordered_map<WorkerId, Resources> sums;
  std::unordered_map<WorkerId, std::set<JobId>> members;
  for (const auto& kv : jobs_) {
    const Job& j = kv.second;
    if (j.attempts > j.spec.max_attempts) err << "job " << j.id << " exceeded max_attempts; ";
    if (j.state == JobState::PENDING && !in_queue.count(j.id))
      err << "PENDING job " << j.id << " not queued; ";
    if (IsActive(j)) {
      if (FindWorker(j.assigned_worker) == nullptr) {
        err << "active job " << j.id << " has no worker; ";
      } else {
        sums[j.assigned_worker] += j.spec.request;
        members[j.assigned_worker].insert(j.id);
      }
    }
  }
  for (const Worker& w : workers_) {
    if (sums[w.id] != w.allocated) err << "worker " << w.id << " allocated mismatch; ";
    if (members[w.id] != w.jobs) err << "worker " << w.id << " job set mismatch; ";
    if (w.allocated.cpu_millis < 0 || w.allocated.mem_mib < 0)
      err << "worker " << w.id << " negative allocation; ";
    if (!w.allocated.FitsIn(w.capacity)) err << "worker " << w.id << " over capacity; ";
  }
  return err.str();
}

}  // namespace kestrel
