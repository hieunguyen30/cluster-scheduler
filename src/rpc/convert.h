// Conversions between wire (protobuf) types and core types.
#pragma once

#include "core/cluster_state.h"
#include "kestrel.pb.h"

namespace kestrel::rpc {

inline Resources FromProto(const v1::Resources& r) { return {r.cpu_millis(), r.mem_mib()}; }
inline void ToProto(const Resources& r, v1::Resources* out) {
  out->set_cpu_millis(r.cpu_millis);
  out->set_mem_mib(r.mem_mib);
}

inline JobSpec FromProto(const v1::JobSpec& s, int default_max_attempts) {
  JobSpec out;
  out.command = s.command();
  out.args.assign(s.args().begin(), s.args().end());
  out.request = FromProto(s.request());
  out.max_attempts = s.max_attempts() > 0 ? s.max_attempts() : default_max_attempts;
  out.request_id = s.request_id();
  return out;
}
inline void ToProto(const JobSpec& s, v1::JobSpec* out) {
  out->set_command(s.command);
  for (const auto& a : s.args) out->add_args(a);
  ToProto(s.request, out->mutable_request());
  out->set_max_attempts(s.max_attempts);
  out->set_request_id(s.request_id);
}

inline v1::JobState ToProto(JobState s) {
  switch (s) {
    case JobState::PENDING: return v1::JOB_STATE_PENDING;
    case JobState::SCHEDULED: return v1::JOB_STATE_SCHEDULED;
    case JobState::RUNNING: return v1::JOB_STATE_RUNNING;
    case JobState::SUCCEEDED: return v1::JOB_STATE_SUCCEEDED;
    case JobState::FAILED: return v1::JOB_STATE_FAILED;
    case JobState::LOST: return v1::JOB_STATE_LOST;
    case JobState::CANCELLED: return v1::JOB_STATE_CANCELLED;
  }
  return v1::JOB_STATE_UNSPECIFIED;
}

inline v1::WorkerStatus ToProto(WorkerStatus s) {
  switch (s) {
    case WorkerStatus::ALIVE: return v1::WORKER_STATUS_ALIVE;
    case WorkerStatus::SUSPECT: return v1::WORKER_STATUS_SUSPECT;
    case WorkerStatus::DEAD: return v1::WORKER_STATUS_DEAD;
  }
  return v1::WORKER_STATUS_UNSPECIFIED;
}

inline void ToProto(const Job& j, const std::string& worker_name, v1::JobInfo* out) {
  out->set_id(j.id);
  ToProto(j.spec, out->mutable_spec());
  out->set_state(ToProto(j.state));
  out->set_attempts(j.attempts);
  out->set_assigned_worker(j.assigned_worker);
  out->set_assigned_worker_name(worker_name);
  out->set_generation(j.generation);
  out->set_submitted_us(j.submitted);
  out->set_first_scheduled_us(j.first_scheduled);
  out->set_started_us(j.started);
  out->set_finished_us(j.finished);
  out->set_exit_code(j.exit_code);
}

inline void ToProto(const Worker& w, v1::WorkerInfo* out) {
  out->set_id(w.id);
  out->set_name(w.name);
  out->set_epoch(w.epoch);
  ToProto(w.capacity, out->mutable_capacity());
  ToProto(w.allocated, out->mutable_allocated());
  out->set_status(ToProto(w.status));
  out->set_last_heartbeat_us(w.last_heartbeat);
  for (JobId id : w.jobs) out->add_jobs(id);
}

template <typename Req>
std::vector<RunningReport> RunningFrom(const Req& req) {
  std::vector<RunningReport> v;
  for (const auto& r : req.running()) v.push_back({r.job_id(), r.generation()});
  return v;
}
template <typename Req>
std::vector<CompletedReport> CompletedFrom(const Req& req) {
  std::vector<CompletedReport> v;
  for (const auto& c : req.completed()) v.push_back({c.job_id(), c.generation(), c.exit_code()});
  return v;
}

}  // namespace kestrel::rpc
