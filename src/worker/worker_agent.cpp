#include "worker/worker_agent.h"

#include <fcntl.h>
#include <grpcpp/grpcpp.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

#include "common/log.h"
#include "kestrel.grpc.pb.h"
#include "rpc/convert.h"

extern char** environ;

namespace kestrel {

class WorkerAgent::Rpc {
 public:
  explicit Rpc(const std::string& addr) {
    grpc::ChannelArguments args;
    // After a control-plane restart we want to reconnect within ~a heartbeat, not after gRPC's
    // default exponential backoff (up to 2 minutes).
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    channel = grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args);
    stub = v1::WorkerService::NewStub(channel);
  }
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<v1::WorkerService::Stub> stub;
};

WorkerAgent::WorkerAgent(WorkerConfig cfg)
    : cfg_(std::move(cfg)), rpc_(std::make_unique<Rpc>(cfg_.control_addr)) {}

WorkerAgent::~WorkerAgent() { Stop(); }

void WorkerAgent::Start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_) return;
  started_ = true;
  hb_thread_ = std::thread([this] { HeartbeatLoop(); });
  reaper_thread_ = std::thread([this] { ReaperLoop(); });
}

void WorkerAgent::Stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_ || stop_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (hb_thread_.joinable()) hb_thread_.join();
  if (reaper_thread_.joinable()) reaper_thread_.join();
  KillAllAndReap();
}

size_t WorkerAgent::RunningCount() {
  std::lock_guard<std::mutex> lk(mu_);
  return running_.size();
}
std::vector<JobId> WorkerAgent::RunningJobs() {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<JobId> v;
  for (auto& kv : running_) v.push_back(kv.first);
  return v;
}
WorkerId WorkerAgent::worker_id() {
  std::lock_guard<std::mutex> lk(mu_);
  return id_;
}
Epoch WorkerAgent::epoch() {
  std::lock_guard<std::mutex> lk(mu_);
  return epoch_;
}
bool WorkerAgent::registered() {
  std::lock_guard<std::mutex> lk(mu_);
  return registered_;
}

void WorkerAgent::HeartbeatLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!stop_) {
    lk.unlock();
    Tick();  // never called with mu_ held: it does network I/O
    lk.lock();
    if (stop_) break;
    cv_.wait_for(lk, cfg_.heartbeat_interval, [this] { return stop_; });
  }
}

void WorkerAgent::ReaperLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!stop_) {
    cv_.wait_for(lk, std::chrono::milliseconds(10), [this] { return stop_; });
    if (stop_) break;
    ReapLocked();
  }
}

bool WorkerAgent::DoRegister() {
  v1::RegisterRequest req;
  req.set_name(cfg_.name);
  rpc::ToProto(cfg_.capacity, req.mutable_capacity());
  {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [id, r] : running_) {
      auto* rj = req.add_running();
      rj->set_job_id(id);
      rj->set_generation(r.generation);
    }
    for (const auto& c : completed_) {
      auto* cj = req.add_completed();
      cj->set_job_id(c.job);
      cj->set_generation(c.generation);
      cj->set_exit_code(c.exit_code);
    }
  }
  v1::RegisterResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + cfg_.rpc_timeout);
  grpc::Status st = rpc_->stub->Register(&ctx, req, &resp);
  if (!st.ok()) {
    log::Write(log::Level::DEBUG, "worker", "register failed",
               {{"name", cfg_.name}, {"error", st.error_message()}});
    return false;
  }
  std::lock_guard<std::mutex> lk(mu_);
  id_ = resp.worker_id();
  epoch_ = resp.epoch();
  registered_ = true;
  for (uint64_t id : resp.kill_jobs()) KillJobLocked(id);
  for (uint64_t id : resp.acked_jobs())
    completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
                                    [id](const CompletedReport& c) { return c.job == id; }),
                     completed_.end());
  log::Write(log::Level::INFO, "worker", "registered",
             {{"name", cfg_.name},
              {"worker", log::S(id_)},
              {"epoch", log::S(epoch_)},
              {"kill", log::S(static_cast<uint64_t>(resp.kill_jobs_size()))}});
  return true;
}

void WorkerAgent::Tick() {
  if (partitioned_.load()) return;
  if (!registered()) {
    DoRegister();
    return;
  }
  v1::HeartbeatRequest req;
  {
    std::lock_guard<std::mutex> lk(mu_);
    req.set_worker_id(id_);
    req.set_epoch(epoch_);
    for (auto& [id, r] : running_) {
      auto* rj = req.add_running();
      rj->set_job_id(id);
      rj->set_generation(r.generation);
    }
    for (const auto& c : completed_) {
      auto* cj = req.add_completed();
      cj->set_job_id(c.job);
      cj->set_generation(c.generation);
      cj->set_exit_code(c.exit_code);
    }
  }
  v1::HeartbeatResponse resp;
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + cfg_.rpc_timeout);
  grpc::Status st = rpc_->stub->Heartbeat(&ctx, req, &resp);
  if (!st.ok()) {
    log::Write(log::Level::DEBUG, "worker", "heartbeat failed",
               {{"name", cfg_.name}, {"error", st.error_message()}});
    return;  // keep running jobs; try again next tick
  }
  if (partitioned_.load()) return;  // partition began mid-call: drop the response
  bool reregister = false;
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (resp.reregister()) {
      registered_ = false;
      reregister = true;
    } else {
      for (uint64_t id : resp.acked_jobs())
        completed_.erase(std::remove_if(completed_.begin(), completed_.end(),
                                        [id](const CompletedReport& c) { return c.job == id; }),
                         completed_.end());
      for (uint64_t id : resp.kill_jobs()) KillJobLocked(id);
      for (const v1::Assignment& a : resp.assignments()) {
        const JobId id = a.job_id();
        auto it = running_.find(id);
        if (it != running_.end() && it->second.generation == a.generation()) continue;
        bool finished_already = std::any_of(
            completed_.begin(), completed_.end(),
            [&](const CompletedReport& c) { return c.job == id && c.generation == a.generation(); });
        if (finished_already) continue;
        if (it != running_.end()) KillJobLocked(id);  // older attempt of the same job
        StartJobLocked(id, a.generation(), rpc::FromProto(a.spec(), 3));
      }
    }
  }
  // Fencing: our epoch is stale (or we were declared dead). Re-register immediately,
  // reporting what we still run so the control plane can adopt or kill each job.
  if (reregister) {
    log::Write(log::Level::WARN, "worker", "told to re-register", {{"name", cfg_.name}});
    DoRegister();
  }
}

void WorkerAgent::StartJobLocked(JobId id, uint64_t gen, const JobSpec& spec) {
  std::vector<std::string> argv_s;
  argv_s.push_back(spec.command);
  for (const auto& a : spec.args) argv_s.push_back(a);
  std::vector<char*> argv;
  for (auto& s : argv_s) argv.push_back(s.data());
  argv.push_back(nullptr);

  posix_spawn_file_actions_t fa;
  posix_spawnattr_t attr;
  posix_spawn_file_actions_init(&fa);
  posix_spawnattr_init(&attr);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  if (!cfg_.log_dir.empty()) {
    std::string path = cfg_.log_dir + "/job-" + std::to_string(id) + "-g" + std::to_string(gen) + ".log";
    posix_spawn_file_actions_addopen(&fa, 1, path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  } else {
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
  }
  posix_spawn_file_actions_adddup2(&fa, 1, 2);
  // Own process group so a kill reaches the whole job tree; clean signal mask for the child.
  sigset_t none;
  sigemptyset(&none);
  posix_spawnattr_setsigmask(&attr, &none);
  posix_spawnattr_setpgroup(&attr, 0);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);

  pid_t pid = 0;
  int rc = posix_spawnp(&pid, argv[0], &fa, &attr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  if (rc != 0) {
    log::Write(log::Level::WARN, "worker", "spawn failed",
               {{"job", log::S(id)}, {"cmd", spec.command}, {"error", std::strerror(rc)}});
    completed_.push_back({id, gen, 127});
    return;
  }
  running_[id] = Running{gen, pid};
  log::Write(log::Level::INFO, "worker", "job started",
             {{"job", log::S(id)}, {"gen", log::S(gen)}, {"pid", log::S(static_cast<int>(pid))}});
}

void WorkerAgent::KillJobLocked(JobId id) {
  auto it = running_.find(id);
  if (it == running_.end()) return;
  pid_t pid = it->second.pid;
  log::Write(log::Level::INFO, "worker", "killing job",
             {{"job", log::S(id)}, {"gen", log::S(it->second.generation)}});
  ::kill(-pid, SIGTERM);
  dying_.push_back({pid, std::chrono::steady_clock::now() + cfg_.kill_grace, false});
  running_.erase(it);  // no completion report: the control plane already disowned it
}

void WorkerAgent::ReapLocked() {
  for (auto it = running_.begin(); it != running_.end();) {
    int status = 0;
    pid_t r = ::waitpid(it->second.pid, &status, WNOHANG);
    if (r == it->second.pid || (r < 0 && errno == ECHILD)) {
      int code = 255;
      if (r > 0) code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      completed_.push_back({it->first, it->second.generation, code});
      log::Write(log::Level::INFO, "worker", "job exited",
                 {{"job", log::S(it->first)}, {"gen", log::S(it->second.generation)},
                  {"exit_code", log::S(code)}});
      it = running_.erase(it);
    } else {
      ++it;
    }
  }
  const auto now = std::chrono::steady_clock::now();
  for (auto it = dying_.begin(); it != dying_.end();) {
    int status = 0;
    pid_t r = ::waitpid(it->pid, &status, WNOHANG);
    if (r == it->pid || (r < 0 && errno == ECHILD)) {
      it = dying_.erase(it);
      continue;
    }
    if (!it->sigkilled && now >= it->kill_at) {
      ::kill(-it->pid, SIGKILL);
      it->sigkilled = true;
    }
    ++it;
  }
}

void WorkerAgent::KillAllAndReap() {
  std::lock_guard<std::mutex> lk(mu_);
  for (auto& [id, r] : running_) {
    ::kill(-r.pid, SIGKILL);
    int status = 0;
    ::waitpid(r.pid, &status, 0);
  }
  running_.clear();
  for (auto& d : dying_) {
    ::kill(-d.pid, SIGKILL);
    int status = 0;
    ::waitpid(d.pid, &status, 0);
  }
  dying_.clear();
}

}  // namespace kestrel
