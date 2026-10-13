// kestrel_control: the Kestrel control-plane process.
#include <signal.h>

#include <cstdio>
#include <thread>

#include "common/flags.h"
#include "common/log.h"
#include "control/control_plane.h"
#include "control/grpc_server.h"
#include "control/metrics.h"
#include "wal/wal.h"

using namespace kestrel;

namespace {
void Usage() {
  std::fprintf(stderr,
               "usage: kestrel_control [--listen 0.0.0.0:7000] [--policy best-fit|first-fit|round-robin]\n"
               "                       [--suspect-after-ms 3000] [--dead-after-ms 5000]\n"
               "                       [--detector-interval-ms 250] [--log-level debug|info|warn]\n"
               "                       [--wal-path FILE] [--wal-sync every|interval|none]\n"
               "                       [--wal-interval-ms 10]\n");
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

  // Persistence: replay the WAL (if any) BEFORE serving a single RPC.
  std::unique_ptr<Wal> wal;
  std::vector<Record> recovered;
  const std::string wal_path = f.Str("wal-path");
  if (!wal_path.empty()) {
    WalOptions wo;
    const std::string sync = f.Str("wal-sync", "every");
    if (sync == "every") wo.sync = WalSync::EVERY;
    else if (sync == "interval") wo.sync = WalSync::INTERVAL;
    else if (sync == "none") wo.sync = WalSync::NONE;
    else Usage();
    wo.interval_ms = static_cast<int>(f.Int("wal-interval-ms", 10));
    WalRecoveryInfo info;
    auto w = Wal::Open(wal_path, wo, &recovered, &info);
    if (!w.ok()) {
      std::fprintf(stderr, "cannot open WAL %s: %s\n", wal_path.c_str(), w.status().message().c_str());
      return 1;
    }
    wal = std::move(w.value());
    log::Write(log::Level::INFO, "wal", "opened",
               {{"path", wal_path},
                {"frames", log::S(info.frames)},
                {"records", log::S(info.records)},
                {"torn_bytes_truncated", log::S(info.torn_bytes_truncated)},
                {"sync", sync}});
  }

  ControlPlane cp(cfg, wal.get());
  if (wal) cp.Recover(recovered);
  cp.Start();
  MetricsServer metrics([&] { return RenderMetrics(cp, wal.get()); });
  const int metrics_port = static_cast<int>(f.Int("metrics-port", 9090));
  if (metrics_port > 0) {
    int bound = metrics.Start("0.0.0.0", metrics_port);
    if (bound < 0) {
      std::fprintf(stderr, "cannot listen on metrics port %d\n", metrics_port);
      return 1;
    }
    log::Write(log::Level::INFO, "control", "metrics listening", {{"port", log::S(bound)}});
  }
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
