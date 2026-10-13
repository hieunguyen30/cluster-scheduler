#include "control/control_plane.h"

#include <chrono>

#include "common/log.h"

namespace kestrel {

namespace {
TimeUs WallClockUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}
}  // namespace

ControlPlane::ControlPlane(ControlPlaneConfig cfg, Journal* journal)
    : cfg_(std::move(cfg)),
      state_(journal),
      scheduler_(MakePolicy(cfg_.policy) ? MakePolicy(cfg_.policy) : MakePolicy("best-fit")) {
  if (!cfg_.clock) cfg_.clock = WallClockUs;
}

ControlPlane::~ControlPlane() { Stop(); }

void ControlPlane::Recover(std::span<const Record> records) {
  std::lock_guard<std::mutex> lk(mu_);
  for (const Record& r : records) state_.Replay(r);
  // last_heartbeat in the future == "not silent yet": ghosts get dead_after + grace from now.
  state_.FinishReplay(cfg_.clock() + cfg_.recovery_grace);
  log::Write(log::Level::INFO, "control", "recovered state from journal",
             {{"records", log::S(static_cast<uint64_t>(records.size()))},
              {"jobs", log::S(static_cast<uint64_t>(state_.JobCount()))},
              {"replay_skipped", log::S(state_.counters().replay_skipped)}});
}

void ControlPlane::Start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_) return;
  started_ = true;
  stop_ = false;
  sched_thread_ = std::thread([this] { SchedulerLoop(); });
  detector_thread_ = std::thread([this] { DetectorLoop(); });
}

void ControlPlane::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_ || stop_) return;
    stop_ = true;
  }
  sched_cv_.notify_all();
  detector_cv_.notify_all();
  if (sched_thread_.joinable()) sched_thread_.join();
  if (detector_thread_.joinable()) detector_thread_.join();
}

void ControlPlane::WakeScheduler() {
  wake_ = true;
  sched_cv_.notify_one();
}

void ControlPlane::SchedulerLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!stop_) {
    // Wake on: new job, freed capacity, new/returning worker, requeued job -- or the safety tick.
    sched_cv_.wait_for(lk, std::chrono::microseconds(cfg_.scheduler_tick),
                       [this] { return stop_ || wake_; });
    if (stop_) break;
    wake_ = false;
    const TimeUs now = cfg_.clock();
    for (JobId id : scheduler_.RunPass(state_, now)) {
      const Job* j = state_.FindJob(id);
      const Worker* w = state_.FindWorker(j->assigned_worker);
      if (j->attempts == 1)  // first placement only: reschedules would double count one job
        sched_latency_.Observe(static_cast<double>(now - j->submitted) / 1e6);
      log::Write(log::Level::INFO, "sched", "assigned",
                 {{"job", log::S(id)},
                  {"worker", log::S(j->assigned_worker)},
                  {"worker_name", w ? w->name : ""},
                  {"gen", log::S(j->generation)},
                  {"attempt", log::S(j->attempts)},
                  {"wait_ms", log::S((now - j->submitted) / 1000)}});
    }
  }
}

void ControlPlane::DetectorLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  TimeUs last_tick = cfg_.clock();
  while (!stop_) {
    detector_cv_.wait_for(lk, std::chrono::microseconds(cfg_.detector_interval),
                          [this] { return stop_; });
    if (stop_) break;
    const TimeUs now = cfg_.clock();
    // Self-monitoring. This tick is late if some other thread held mu_ for a long time (the
    // journal fsync runs under it) or the whole process was paused. While we were stuck, every
    // heartbeat handler was stuck too, so "silence" over that window is OUR fault, not the
    // workers'. Without this credit a slow fsync makes the control plane declare every worker
    // dead the moment it wakes up, before it has processed the heartbeats queued behind the lock.
    const TimeUs late = now - last_tick - cfg_.detector_interval;
    if (late > cfg_.detector_interval) {
      state_.CreditSilence(late, now);
      ++detector_stalls_;
      log::Write(log::Level::WARN, "detector", "detector was stalled; crediting workers",
                 {{"stall_ms", log::S(late / 1000)}});
    }
    last_tick = now;
    // Capture the pre-tick status so SUSPECT transitions are logged too.
    std::vector<std::pair<WorkerId, WorkerStatus>> before;
    for (const Worker& w : state_.Workers()) before.emplace_back(w.id, w.status);
    std::vector<WorkerId> dead = state_.CheckWorkers(now, cfg_.suspect_after, cfg_.dead_after);
    for (const auto& [id, old_status] : before) {
      const Worker* w = state_.FindWorker(id);
      if (w->status != old_status) {
        log::Write(w->status == WorkerStatus::DEAD ? log::Level::WARN : log::Level::INFO,
                   "detector", "worker status change",
                   {{"worker", log::S(id)},
                    {"name", w->name},
                    {"from", ToString(old_status)},
                    {"to", ToString(w->status)},
                    {"silent_ms", log::S((now - w->last_heartbeat) / 1000)}});
      }
    }
    if (!dead.empty()) WakeScheduler();  // their jobs are back in the queue
  }
}

Result<JobId> ControlPlane::SubmitJob(JobSpec spec) {
  std::lock_guard<std::mutex> lk(mu_);
  if (spec.max_attempts <= 0) spec.max_attempts = cfg_.default_max_attempts;
  auto r = state_.Submit(std::move(spec), cfg_.clock());
  if (r.ok()) {
    log::Write(log::Level::INFO, "api", "job submitted", {{"job", log::S(r.value())}});
    WakeScheduler();
  }
  return r;
}

Result<Job> ControlPlane::GetJob(JobId id, std::string* worker_name) {
  std::lock_guard<std::mutex> lk(mu_);
  const Job* j = state_.FindJob(id);
  if (j == nullptr) return Status::Error("no such job");
  if (worker_name != nullptr) {
    const Worker* w = state_.FindWorker(j->assigned_worker);
    *worker_name = w ? w->name : "";
  }
  return *j;
}

std::vector<Job> ControlPlane::ListJobs(std::optional<JobState> filter, size_t limit) {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<Job> out;
  for (Job& j : state_.ListJobs()) {
    if (filter && j.state != *filter) continue;
    out.push_back(std::move(j));
    if (limit > 0 && out.size() >= limit) break;
  }
  return out;
}

Status ControlPlane::CancelJob(JobId id) {
  std::lock_guard<std::mutex> lk(mu_);
  Status s = state_.Cancel(id, cfg_.clock());
  if (s.ok()) {
    log::Write(log::Level::INFO, "api", "job cancelled", {{"job", log::S(id)}});
    WakeScheduler();  // capacity may have been freed
  }
  return s;
}

std::vector<Worker> ControlPlane::ListWorkers() {
  std::lock_guard<std::mutex> lk(mu_);
  auto ws = state_.Workers();
  return std::vector<Worker>(ws.begin(), ws.end());
}

Result<RegisterResult> ControlPlane::Register(const std::string& name, Resources capacity,
                                              std::span<const RunningReport> running,
                                              std::span<const CompletedReport> completed) {
  std::lock_guard<std::mutex> lk(mu_);
  auto r = state_.RegisterWorker(name, capacity, running, completed, cfg_.clock());
  if (r.ok()) {
    log::Write(log::Level::INFO, "control", "worker registered",
               {{"worker", log::S(r.value().worker)},
                {"name", name},
                {"epoch", log::S(r.value().epoch)},
                {"reported_running", log::S(static_cast<uint64_t>(running.size()))},
                {"kill", log::S(static_cast<uint64_t>(r.value().kill.size()))}});
    WakeScheduler();
  }
  return r;
}

HeartbeatResult ControlPlane::Heartbeat(WorkerId worker, Epoch epoch,
                                        std::span<const RunningReport> running,
                                        std::span<const CompletedReport> completed) {
  std::lock_guard<std::mutex> lk(mu_);
  HeartbeatResult r = state_.Heartbeat(worker, epoch, running, completed, cfg_.clock());
  if (r.reregister) {
    log::Write(log::Level::WARN, "control", "heartbeat rejected (stale epoch or dead worker)",
               {{"worker", log::S(worker)}, {"epoch", log::S(epoch)}});
  } else if (!completed.empty()) {
    WakeScheduler();  // completions free capacity
  }
  return r;
}

ClusterStats ControlPlane::Snapshot() {
  std::lock_guard<std::mutex> lk(mu_);
  ClusterStats s;
  s.pending_depth = state_.Pending().size();
  for (const Job& j : state_.ListJobs()) ++s.jobs_by_state[static_cast<int>(j.state)];
  for (const Worker& w : state_.Workers()) {
    switch (w.status) {
      case WorkerStatus::ALIVE:
        ++s.workers_alive;
        s.capacity_alive += w.capacity;
        s.allocated_alive += w.allocated;
        break;
      case WorkerStatus::SUSPECT: ++s.workers_suspect; break;
      case WorkerStatus::DEAD: ++s.workers_dead; break;
    }
  }
  s.counters = state_.counters();
  s.detector_stalls = detector_stalls_;
  return s;
}

void ControlPlane::WithState(const std::function<void(const ClusterState&)>& fn) {
  std::lock_guard<std::mutex> lk(mu_);
  fn(state_);
}

}  // namespace kestrel
