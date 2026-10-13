// Prometheus text-format metrics and a minimal HTTP endpoint (DESIGN.md section 7).
// Hand-rolled instead of prometheus-cpp: ~150 lines, no extra dependency to build in the image.
#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "control/control_plane.h"
#include "wal/wal.h"

namespace kestrel {

// Renders every Kestrel metric in Prometheus exposition format. `wal` may be null.
std::string RenderMetrics(ControlPlane& cp, const Wal* wal);

// Serves GET /metrics (text/plain; version=0.0.4) and GET /healthz on a TCP port.
// One request at a time on one thread: scrapes are rare and tiny.
class MetricsServer {
 public:
  explicit MetricsServer(std::function<std::string()> render) : render_(std::move(render)) {}
  ~MetricsServer() { Stop(); }
  // Returns the bound port (pass 0 for an ephemeral one), or -1 on failure.
  int Start(const std::string& bind_addr, int port);
  void Stop();

 private:
  void Loop();
  std::function<std::string()> render_;
  int listen_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace kestrel
