// In-process cluster for integration tests: a real ControlPlane + gRPC server on an ephemeral
// port, real WorkerAgents (real subprocesses), and a gRPC client. Timings are shortened so
// failure detection takes about a second.
#pragma once

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "control/control_plane.h"
#include "control/grpc_server.h"
#include "kestrel.grpc.pb.h"
#include "worker/worker_agent.h"

namespace kestrel::testing_util {

inline bool WaitFor(const std::function<bool()>& pred,
                    std::chrono::milliseconds timeout = std::chrono::seconds(30),
                    std::chrono::milliseconds step = std::chrono::milliseconds(10)) {
  auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) return true;
    std::this_thread::sleep_for(step);
  }
  return pred();
}

// Sanitizer builds are several times slower, which delays heartbeats and can trip a short dead
// timeout. KESTREL_TIME_SCALE (set by scripts/build.sh for asan/tsan) stretches every failure-
// detection threshold and every sleep that is compared against one. Heartbeat period is unscaled.
inline double TimeScale() {
  const char* e = std::getenv("KESTREL_TIME_SCALE");
  double s = e ? std::atof(e) : 1.0;
  return s >= 1.0 ? s : 1.0;
}
inline TimeUs Scaled(TimeUs t) { return static_cast<TimeUs>(static_cast<double>(t) * TimeScale()); }
// Seconds as a string for use inside `sleep` in job scripts.
inline std::string ScaledSeconds(double s) { return std::to_string(s * TimeScale()); }

struct HarnessOptions {
  std::string policy = "best-fit";
  std::chrono::milliseconds heartbeat{100};
  TimeUs suspect_after = Scaled(500 * kMillisecond);
  TimeUs dead_after = Scaled(1000 * kMillisecond);
  TimeUs detector_interval = 50 * kMillisecond;
};

class Harness {
 public:
  explicit Harness(HarnessOptions opts = {}, Journal* journal = nullptr) : opts_(opts) {
    ControlPlaneConfig cfg;
    cfg.policy = opts.policy;
    cfg.suspect_after = opts.suspect_after;
    cfg.dead_after = opts.dead_after;
    cfg.detector_interval = opts.detector_interval;
    cfg.scheduler_tick = 100 * kMillisecond;
    cp = std::make_unique<ControlPlane>(cfg, journal);
  }
  ~Harness() { Shutdown(); }

  // Starts the control plane (call after any Recover()).
  void StartControl(const std::string& listen = "127.0.0.1:0") {
    cp->Start();
    server = std::make_unique<ControlPlaneServer>(*cp);
    auto port_r = server->Start(listen);
    if (!port_r.ok()) std::abort();
    port = port_r.value();
    Connect();
  }

  void Connect() {
    channel = grpc::CreateChannel(Address(), grpc::InsecureChannelCredentials());
    stub = v1::SchedulerService::NewStub(channel);
  }

  std::string Address() const { return "127.0.0.1:" + std::to_string(port); }

  WorkerAgent* AddWorker(const std::string& name, Resources cap = {4000, 8192}) {
    WorkerConfig wc;
    wc.name = name;
    wc.control_addr = Address();
    wc.capacity = cap;
    wc.heartbeat_interval = opts_.heartbeat;
    wc.rpc_timeout = std::chrono::milliseconds(500);
    wc.kill_grace = std::chrono::milliseconds(300);
    workers.push_back(std::make_unique<WorkerAgent>(wc));
    workers.back()->Start();
    agents[name] = workers.back().get();
    return workers.back().get();
  }

  std::string WorkerNameOf(JobId id) {
    std::string name;
    auto r = cp->GetJob(id, &name);
    return name;
  }

  WorkerStatus StatusOf(const std::string& name) {
    for (const Worker& w : cp->ListWorkers())
      if (w.name == name) return w.status;
    return WorkerStatus::DEAD;
  }

  Epoch EpochOf(const std::string& name) {
    for (const Worker& w : cp->ListWorkers())
      if (w.name == name) return w.epoch;
    return 0;
  }

  // Waits until `n` workers are registered and ALIVE.
  bool WaitForWorkers(size_t n) {
    return WaitFor([&] {
      size_t alive = 0;
      for (const Worker& w : cp->ListWorkers()) alive += w.status == WorkerStatus::ALIVE;
      return alive >= n;
    });
  }

  JobId Submit(const std::string& cmd, std::vector<std::string> args, int64_t cpu = 100,
               int64_t mem = 64, int attempts = 0, const std::string& request_id = "") {
    v1::SubmitJobRequest req;
    auto* s = req.mutable_spec();
    s->set_command(cmd);
    for (auto& a : args) s->add_args(a);
    s->mutable_request()->set_cpu_millis(cpu);
    s->mutable_request()->set_mem_mib(mem);
    s->set_max_attempts(attempts);
    s->set_request_id(request_id);
    grpc::ClientContext ctx;
    v1::SubmitJobResponse resp;
    grpc::Status st = stub->SubmitJob(&ctx, req, &resp);
    if (!st.ok()) {
      ADD_FAILURE() << "SubmitJob: " << st.error_message();
      return 0;
    }
    return resp.job_id();
  }

  JobId Sh(const std::string& script, int64_t cpu = 100, int64_t mem = 64, int attempts = 0) {
    return Submit("sh", {"-c", script}, cpu, mem, attempts);
  }

  Job Get(JobId id) {
    auto r = cp->GetJob(id);
    if (!r.ok()) return Job{};
    return r.value();
  }

  bool WaitState(JobId id, JobState s, std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
    return WaitFor([&] { return Get(id).state == s; }, timeout);
  }

  std::string CheckInvariants() {
    std::string out;
    cp->WithState([&](const ClusterState& st) { out = st.CheckInvariants(); });
    return out;
  }

  void Shutdown() {
    for (auto& w : workers) w->Stop();
    workers.clear();
    if (server) server->Shutdown();
    server.reset();
    cp->Stop();
  }

  HarnessOptions opts_;
  std::unique_ptr<ControlPlane> cp;
  std::unique_ptr<ControlPlaneServer> server;
  int port = 0;
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<v1::SchedulerService::Stub> stub;
  std::vector<std::unique_ptr<WorkerAgent>> workers;
  std::map<std::string, WorkerAgent*> agents;  // latest agent per worker name
};

}  // namespace kestrel::testing_util
