// kestrel_control: the Kestrel control-plane process.
#include <signal.h>

#include <cstdio>
#include <thread>

#include "common/flags.h"
#include "common/log.h"
#include "control/control_plane.h"
#include "control/grpc_server.h"

using namespace kestrel;

namespace {
void Usage() {
  std::fprintf(stderr,
               "usage: kestrel_control [--listen 0.0.0.0:7000] [--policy best-fit|first-fit|round-robin]\n"
               "                       [--suspect-after-ms 3000] [--dead-after-ms 5000]\n"
               "                       [--detector-interval-ms 250] [--log-level debug|info|warn]\n");
  std::exit(2);
}
}  // namespace

int main(int argc, char** argv) {
  // Block SIGINT/SIGTERM before any thread exists so every thread inherits the mask; one
  // dedicated thread (below) receives them with sigwait.
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);

  Flags f(argc, argv);
  if (f.Has("help")) Usage();
  std::string lvl = f.Str("log-level", "info");
  log::SetLevel(lvl == "debug" ? log::Level::DEBUG : lvl == "warn" ? log::Level::WARN
                                                                    : log::Level::INFO);
  ControlPlaneConfig cfg;
  cfg.policy = f.Str("policy", "best-fit");
  if (!MakePolicy(cfg.policy)) {
    std::fprintf(stderr, "unknown policy %s\n", cfg.policy.c_str());
    return 2;
  }
  cfg.suspect_after = f.Int("suspect-after-ms", 3000) * kMillisecond;
  cfg.dead_after = f.Int("dead-after-ms", 5000) * kMillisecond;
  cfg.detector_interval = f.Int("detector-interval-ms", 250) * kMillisecond;

  ControlPlane cp(cfg);
  cp.Start();
  ControlPlaneServer server(cp);
  auto port = server.Start(f.Str("listen", "0.0.0.0:7000"));
  if (!port.ok()) {
    std::fprintf(stderr, "%s\n", port.status().message().c_str());
    return 1;
  }
  log::Write(log::Level::INFO, "control", "listening",
             {{"port", log::S(port.value())}, {"policy", cfg.policy}});

  std::thread sig_thread([&] {
    int sig = 0;
    sigwait(&set, &sig);
    log::Write(log::Level::INFO, "control", "shutting down");
    server.Shutdown();
  });
  server.Wait();
  pthread_kill(sig_thread.native_handle(), SIGTERM);  // unblock sigwait if Wait returned first
  sig_thread.join();
  cp.Stop();
  return 0;
}
