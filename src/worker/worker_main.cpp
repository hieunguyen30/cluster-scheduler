// kestrel_worker: the Kestrel worker agent process.
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <thread>

#include "common/flags.h"
#include "common/log.h"
#include "worker/worker_agent.h"

using namespace kestrel;

int main(int argc, char** argv) {
  Flags f(argc, argv);
  if (f.Has("help") || !f.Has("control")) {
    std::fprintf(stderr,
                 "usage: kestrel_worker --control host:port [--name NAME] [--cpu-millis 4000]\n"
                 "                      [--mem-mib 8192] [--heartbeat-ms 1000] [--log-dir DIR]\n"
                 "                      [--log-level debug|info|warn]\n");
    return 2;
  }
  std::string lvl = f.Str("log-level", "info");
  log::SetLevel(lvl == "debug" ? log::Level::DEBUG : lvl == "warn" ? log::Level::WARN
                                                                    : log::Level::INFO);
  WorkerConfig cfg;
  char host[256] = "worker";
  gethostname(host, sizeof host);
  cfg.name = f.Str("name", host);  // in Kubernetes the pod name is the hostname
  cfg.control_addr = f.Str("control");
  cfg.capacity = {f.Int("cpu-millis", 4000), f.Int("mem-mib", 8192)};
  cfg.heartbeat_interval = std::chrono::milliseconds(f.Int("heartbeat-ms", 1000));
  cfg.log_dir = f.Str("log-dir");

  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  WorkerAgent agent(cfg);
  agent.Start();
  log::Write(log::Level::INFO, "worker", "started",
             {{"name", cfg.name}, {"control", cfg.control_addr}});
  int sig = 0;
  sigwait(&set, &sig);
  log::Write(log::Level::INFO, "worker", "stopping (killing running jobs)");
  agent.Stop();
  return 0;
}
