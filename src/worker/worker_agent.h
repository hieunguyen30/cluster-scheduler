// Worker agent (DESIGN.md section 2): registers with the control plane, heartbeats, runs
// assigned jobs as subprocesses and reports results.
//
// Protocol notes (see DECISIONS.md D15/D16):
//  * Heartbeats carry the full list of running jobs and every completed-but-unacknowledged
//    result, so every message is idempotent and a lost response costs one interval, not a job.
//  * If the control plane is unreachable the agent keeps its jobs running and keeps retrying.
//    It only kills a job when told to (kill order, or reconcile at re-registration).
#pragma once

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/cluster_state.h"

namespace kestrel {

struct WorkerConfig {
  std::string name;
  std::string control_addr;  // host:port
  Resources capacity{4000, 8192};
  std::chrono::milliseconds heartbeat_interval{1000};
  std::chrono::milliseconds rpc_timeout{1000};
  std::chrono::milliseconds kill_grace{2000};  // SIGTERM -> SIGKILL
  std::string log_dir;  // if set, job stdout/stderr go to <log_dir>/job-<id>-g<gen>.log
};

class WorkerAgent {
 public:
  explicit WorkerAgent(WorkerConfig cfg);
  ~WorkerAgent();

  void Start();
  // Stops the loops and SIGKILLs + reaps every job still running (a clean node shutdown).
  void Stop();

  // Test hook: drop every RPC to the control plane while jobs keep running (a partition).
  void SetPartitioned(bool p) { partitioned_.store(p); }

  size_t RunningCount();
  std::vector<JobId> RunningJobs();
  WorkerId worker_id();
  Epoch epoch();
  bool registered();

 private:
  struct Running {
    uint64_t generation = 0;
    pid_t pid = 0;
  };
  struct Dying {
    pid_t pid = 0;
    std::chrono::steady_clock::time_point kill_at;
    bool sigkilled = false;
  };
  class Rpc;  // hides the gRPC stub from this header

  void HeartbeatLoop();
  void ReaperLoop();
  void Tick();
  void NoteRpcResult(bool ok);
  bool DoRegister();
  void StartJobLocked(JobId id, uint64_t gen, const JobSpec& spec);
  void KillJobLocked(JobId id);
  void ReapLocked();
  void KillAllAndReap();

  WorkerConfig cfg_;
  static constexpr int kRebuildChannelAfterFailures = 3;
  std::unique_ptr<Rpc> rpc_;  // replaced only by the heartbeat thread
  int consecutive_rpc_failures_ = 0;  // heartbeat thread only
  std::atomic<bool> partitioned_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
  bool registered_ = false;
  WorkerId id_ = 0;
  Epoch epoch_ = 0;
  std::map<JobId, Running> running_;
  std::vector<CompletedReport> completed_;  // unacknowledged results
  std::vector<Dying> dying_;
  std::thread hb_thread_, reaper_thread_;
};

}  // namespace kestrel
