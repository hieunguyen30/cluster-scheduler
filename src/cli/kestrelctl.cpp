// kestrelctl: command-line client.
//   kestrelctl [--control host:port] submit --cpu 500 --mem 256 [--attempts 3] [--request-id K]
//                                           [--wait] -- cmd arg...
//   kestrelctl get ID | list [--state RUNNING] | cancel ID | workers | wait ID [--timeout-s 60]
#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdio>
#include <thread>

#include "common/flags.h"
#include "kestrel.grpc.pb.h"

using namespace kestrel;

namespace {

const char* StateName(v1::JobState s) {
  switch (s) {
    case v1::JOB_STATE_PENDING: return "PENDING";
    case v1::JOB_STATE_SCHEDULED: return "SCHEDULED";
    case v1::JOB_STATE_RUNNING: return "RUNNING";
    case v1::JOB_STATE_SUCCEEDED: return "SUCCEEDED";
    case v1::JOB_STATE_FAILED: return "FAILED";
    case v1::JOB_STATE_LOST: return "LOST";
    case v1::JOB_STATE_CANCELLED: return "CANCELLED";
    default: return "?";
  }
}
const char* WorkerStatusName(v1::WorkerStatus s) {
  switch (s) {
    case v1::WORKER_STATUS_ALIVE: return "ALIVE";
    case v1::WORKER_STATUS_SUSPECT: return "SUSPECT";
    case v1::WORKER_STATUS_DEAD: return "DEAD";
    default: return "?";
  }
}
bool Terminal(v1::JobState s) {
  return s == v1::JOB_STATE_SUCCEEDED || s == v1::JOB_STATE_FAILED || s == v1::JOB_STATE_CANCELLED;
}

void PrintJob(const v1::JobInfo& j) {
  std::printf("job %llu  %-9s attempts=%d/%d worker=%s gen=%llu cpu=%lldm mem=%lldMiB exit=%d cmd=%s\n",
              static_cast<unsigned long long>(j.id()), StateName(j.state()), j.attempts(),
              j.spec().max_attempts(), j.assigned_worker_name().c_str(),
              static_cast<unsigned long long>(j.generation()),
              static_cast<long long>(j.spec().request().cpu_millis()),
              static_cast<long long>(j.spec().request().mem_mib()), j.exit_code(),
              j.spec().command().c_str());
}

int Fail(const grpc::Status& s) {
  std::fprintf(stderr, "error: %s (%d)\n", s.error_message().c_str(), static_cast<int>(s.error_code()));
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  Flags f(argc, argv, {"wait"});
  const auto& pos = f.positional();
  if (pos.empty()) {
    std::fprintf(stderr, "usage: kestrelctl [--control host:port] submit|get|list|cancel|workers|wait ...\n");
    return 2;
  }
  auto channel = grpc::CreateChannel(f.Str("control", "127.0.0.1:7000"), grpc::InsecureChannelCredentials());
  auto stub = v1::SchedulerService::NewStub(channel);
  const std::string cmd = pos[0];

  auto wait_for = [&](uint64_t id, int timeout_s) -> int {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (true) {
      grpc::ClientContext ctx;
      v1::GetJobRequest req;
      req.set_job_id(id);
      v1::GetJobResponse resp;
      grpc::Status st = stub->GetJob(&ctx, req, &resp);
      if (!st.ok()) return Fail(st);
      if (Terminal(resp.job().state())) {
        PrintJob(resp.job());
        return resp.job().state() == v1::JOB_STATE_SUCCEEDED ? 0 : 3;
      }
      if (std::chrono::steady_clock::now() > deadline) {
        std::fprintf(stderr, "timeout waiting for job %llu\n", static_cast<unsigned long long>(id));
        return 4;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  };

  if (cmd == "submit") {
    if (pos.size() < 2) {
      std::fprintf(stderr, "submit needs a command after `--`\n");
      return 2;
    }
    v1::SubmitJobRequest req;
    auto* spec = req.mutable_spec();
    spec->set_command(pos[1]);
    for (size_t i = 2; i < pos.size(); ++i) spec->add_args(pos[i]);
    spec->mutable_request()->set_cpu_millis(f.Int("cpu", 100));
    spec->mutable_request()->set_mem_mib(f.Int("mem", 64));
    spec->set_max_attempts(static_cast<int>(f.Int("attempts", 0)));
    spec->set_request_id(f.Str("request-id"));
    grpc::ClientContext ctx;
    v1::SubmitJobResponse resp;
    grpc::Status st = stub->SubmitJob(&ctx, req, &resp);
    if (!st.ok()) return Fail(st);
    std::printf("submitted job %llu\n", static_cast<unsigned long long>(resp.job_id()));
    if (f.Has("wait")) return wait_for(resp.job_id(), static_cast<int>(f.Int("timeout-s", 60)));
    return 0;
  }
  if (cmd == "get" || cmd == "cancel" || cmd == "wait") {
    if (pos.size() < 2) return 2;
    uint64_t id = std::strtoull(pos[1].c_str(), nullptr, 10);
    if (cmd == "wait") return wait_for(id, static_cast<int>(f.Int("timeout-s", 60)));
    grpc::ClientContext ctx;
    if (cmd == "get") {
      v1::GetJobRequest req;
      req.set_job_id(id);
      v1::GetJobResponse resp;
      grpc::Status st = stub->GetJob(&ctx, req, &resp);
      if (!st.ok()) return Fail(st);
      PrintJob(resp.job());
    } else {
      v1::CancelJobRequest req;
      req.set_job_id(id);
      v1::CancelJobResponse resp;
      grpc::Status st = stub->CancelJob(&ctx, req, &resp);
      if (!st.ok()) return Fail(st);
      std::printf("cancelled job %llu\n", static_cast<unsigned long long>(id));
    }
    return 0;
  }
  if (cmd == "list") {
    v1::ListJobsRequest req;
    std::string s = f.Str("state");
    for (int v = v1::JOB_STATE_PENDING; v <= v1::JOB_STATE_CANCELLED; ++v)
      if (!s.empty() && s == StateName(static_cast<v1::JobState>(v))) req.set_state(static_cast<v1::JobState>(v));
    grpc::ClientContext ctx;
    v1::ListJobsResponse resp;
    grpc::Status st = stub->ListJobs(&ctx, req, &resp);
    if (!st.ok()) return Fail(st);
    for (const auto& j : resp.jobs()) PrintJob(j);
    return 0;
  }
  if (cmd == "workers") {
    grpc::ClientContext ctx;
    v1::ListWorkersRequest req;
    v1::ListWorkersResponse resp;
    grpc::Status st = stub->ListWorkers(&ctx, req, &resp);
    if (!st.ok()) return Fail(st);
    for (const auto& w : resp.workers())
      std::printf("worker %llu %-24s %-7s epoch=%llu alloc=%lld/%lldm %lld/%lldMiB jobs=%d\n",
                  static_cast<unsigned long long>(w.id()), w.name().c_str(), WorkerStatusName(w.status()),
                  static_cast<unsigned long long>(w.epoch()), static_cast<long long>(w.allocated().cpu_millis()),
                  static_cast<long long>(w.capacity().cpu_millis()), static_cast<long long>(w.allocated().mem_mib()),
                  static_cast<long long>(w.capacity().mem_mib()), w.jobs_size());
    return 0;
  }
  std::fprintf(stderr, "unknown command %s\n", cmd.c_str());
  return 2;
}
