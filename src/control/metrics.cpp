#include "control/metrics.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <sstream>

namespace kestrel {

namespace {

void Gauge(std::ostringstream& o, const char* name, const char* help, double v,
           const char* labels = "") {
  o << "# HELP " << name << " " << help << "\n# TYPE " << name << " gauge\n"
    << name << labels << " " << v << "\n";
}

void Counter(std::ostringstream& o, const char* name, const char* help, uint64_t v) {
  o << "# HELP " << name << " " << help << "\n# TYPE " << name << " counter\n"
    << name << " " << v << "\n";
}

void Hist(std::ostringstream& o, const char* name, const char* help, const Histogram::Snapshot& s) {
  o << "# HELP " << name << " " << help << "\n# TYPE " << name << " histogram\n";
  for (size_t i = 0; i < s.bounds.size(); ++i)
    o << name << "_bucket{le=\"" << s.bounds[i] << "\"} " << s.cumulative[i] << "\n";
  o << name << "_bucket{le=\"+Inf\"} " << s.cumulative.back() << "\n";
  o << name << "_sum " << s.sum << "\n" << name << "_count " << s.count << "\n";
}

}  // namespace

std::string RenderMetrics(ControlPlane& cp, const Wal* wal) {
  ClusterStats s = cp.Snapshot();
  std::ostringstream o;
  o.precision(9);

  Gauge(o, "kestrel_pending_jobs", "Jobs waiting in the pending queue.",
        static_cast<double>(s.pending_depth));

  o << "# HELP kestrel_jobs Jobs by state.\n# TYPE kestrel_jobs gauge\n";
  static const JobState kStates[] = {JobState::PENDING,   JobState::SCHEDULED, JobState::RUNNING,
                                     JobState::SUCCEEDED, JobState::FAILED,    JobState::LOST,
                                     JobState::CANCELLED};
  for (JobState st : kStates)
    o << "kestrel_jobs{state=\"" << ToString(st) << "\"} " << s.jobs_by_state[static_cast<int>(st)]
      << "\n";

  o << "# HELP kestrel_workers Workers by liveness status.\n# TYPE kestrel_workers gauge\n"
    << "kestrel_workers{status=\"alive\"} " << s.workers_alive << "\n"
    << "kestrel_workers{status=\"suspect\"} " << s.workers_suspect << "\n"
    << "kestrel_workers{status=\"dead\"} " << s.workers_dead << "\n";

  const double cap_cpu = static_cast<double>(s.capacity_alive.cpu_millis);
  const double cap_mem = static_cast<double>(s.capacity_alive.mem_mib);
  Gauge(o, "kestrel_cluster_cpu_allocation_ratio",
        "Allocated / capacity of CPU across ALIVE workers (logical reservations).",
        cap_cpu > 0 ? static_cast<double>(s.allocated_alive.cpu_millis) / cap_cpu : 0);
  Gauge(o, "kestrel_cluster_memory_allocation_ratio",
        "Allocated / capacity of memory across ALIVE workers (logical reservations).",
        cap_mem > 0 ? static_cast<double>(s.allocated_alive.mem_mib) / cap_mem : 0);
  Gauge(o, "kestrel_cluster_cpu_millis_capacity", "CPU capacity of ALIVE workers.", cap_cpu);
  Gauge(o, "kestrel_cluster_memory_mib_capacity", "Memory capacity of ALIVE workers.", cap_mem);

  Counter(o, "kestrel_jobs_submitted_total", "Jobs accepted.", s.counters.jobs_submitted);
  Counter(o, "kestrel_jobs_lost_total", "Job attempts lost because a worker died.",
          s.counters.jobs_lost);
  Counter(o, "kestrel_jobs_rescheduled_total", "Lost attempts that were requeued.",
          s.counters.jobs_rescheduled);
  Counter(o, "kestrel_stale_reports_ignored_total",
          "Reports from superseded attempts ignored by the generation fence.",
          s.counters.stale_reports_ignored);

  Counter(o, "kestrel_detector_stalls_total",
          "Failure-detector ticks that found the control plane itself had been stalled.",
          s.detector_stalls);

  Hist(o, "kestrel_scheduling_latency_seconds",
       "Submit to first assignment (includes waiting for capacity).",
       cp.scheduling_latency().Take());

  if (wal != nullptr) {
    Hist(o, "kestrel_wal_append_latency_seconds", "WAL append latency including fsync.",
         wal->AppendLatency());
    Counter(o, "kestrel_wal_bytes_total", "Bytes appended to the WAL.", wal->bytes_written());
    Counter(o, "kestrel_wal_frames_total", "Frames appended to the WAL.", wal->frames_written());
    Gauge(o, "kestrel_wal_poisoned", "1 if the WAL hit an I/O error and refuses appends.",
          wal->poisoned() ? 1 : 0);
  }
  return o.str();
}

int MetricsServer::Start(const std::string& bind_addr, int port) {
  listen_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (listen_fd_ < 0) return -1;
  int one = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) != 1 ||
      ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      ::listen(listen_fd_, 16) != 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
    return -1;
  }
  socklen_t len = sizeof addr;
  ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  stop_ = false;
  thread_ = std::thread([this] { Loop(); });
  return ntohs(addr.sin_port);
}

void MetricsServer::Stop() {
  stop_ = true;
  if (thread_.joinable()) thread_.join();
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void MetricsServer::Loop() {
  while (!stop_.load()) {
    pollfd p{listen_fd_, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) continue;
    int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) continue;
    timeval tv{2, 0};  // a stuck client must not wedge the endpoint
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    std::string req;
    char buf[1024];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
      ssize_t n = ::recv(fd, buf, sizeof buf, 0);
      if (n <= 0) break;
      req.append(buf, static_cast<size_t>(n));
    }
    std::string status = "404 Not Found", body = "not found\n", ctype = "text/plain";
    if (req.rfind("GET /metrics", 0) == 0) {
      status = "200 OK";
      body = render_();
      ctype = "text/plain; version=0.0.4; charset=utf-8";
    } else if (req.rfind("GET /healthz", 0) == 0) {
      status = "200 OK";
      body = "ok\n";
    }
    std::string resp = "HTTP/1.1 " + status + "\r\nContent-Type: " + ctype +
                       "\r\nContent-Length: " + std::to_string(body.size()) +
                       "\r\nConnection: close\r\n\r\n" + body;
    const char* d = resp.data();
    size_t left = resp.size();
    while (left > 0) {
      ssize_t n = ::send(fd, d, left, MSG_NOSIGNAL);
      if (n <= 0) break;
      d += n;
      left -= static_cast<size_t>(n);
    }
    ::close(fd);
  }
}

}  // namespace kestrel
