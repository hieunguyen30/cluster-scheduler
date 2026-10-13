#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

#include "cluster_harness.h"
#include "control/metrics.h"

using namespace kestrel;
using namespace kestrel::testing_util;

namespace {

// Minimal HTTP/1.1 GET returning "<status line>\n<body>".
std::string HttpGet(int port, const std::string& path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    ::close(fd);
    return "connect failed";
  }
  std::string req = "GET " + path + " HTTP/1.1\r\nHost: x\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string resp;
  char buf[4096];
  ssize_t n;
  while ((n = ::recv(fd, buf, sizeof buf, 0)) > 0) resp.append(buf, static_cast<size_t>(n));
  ::close(fd);
  auto eol = resp.find("\r\n");
  auto body = resp.find("\r\n\r\n");
  if (eol == std::string::npos || body == std::string::npos) return resp;
  return resp.substr(0, eol) + "\n" + resp.substr(body + 4);
}

// Value of the first sample whose line starts with `series` (e.g. kestrel_jobs{state="X"}).
double Sample(const std::string& text, const std::string& series) {
  size_t pos = 0;
  while ((pos = text.find(series + " ", pos)) != std::string::npos) {
    if (pos == 0 || text[pos - 1] == '\n') {
      return std::strtod(text.c_str() + pos + series.size() + 1, nullptr);
    }
    pos += 1;
  }
  return -1;
}

TEST(Metrics, ExposesClusterStateAndHistograms) {
  std::string wal = ::testing::TempDir() + "kestrel_metrics_wal_" + std::to_string(::getpid());
  std::remove(wal.c_str());
  Harness h({}, wal);
  h.StartControl();
  h.AddWorker("w1", {4000, 8192});
  ASSERT_TRUE(h.WaitForWorkers(1));
  for (int i = 0; i < 3; ++i) ASSERT_TRUE(h.WaitState(h.Sh("true"), JobState::SUCCEEDED));
  JobId running = h.Sh("sleep 30", 1000, 2048);
  ASSERT_TRUE(h.WaitState(running, JobState::RUNNING));
  h.Sh("true", 9000, 64);  // fits nowhere: stays pending (submitted AFTER `running`, because a
                           // pending head would block everything behind it: FIFO by design)

  MetricsServer ms([&] { return RenderMetrics(*h.cp, h.wal.get()); });
  int port = ms.Start("127.0.0.1", 0);
  ASSERT_GT(port, 0);

  std::string out = HttpGet(port, "/metrics");
  EXPECT_EQ(out.substr(0, out.find('\n')), "HTTP/1.1 200 OK");
  EXPECT_EQ(Sample(out, "kestrel_jobs{state=\"SUCCEEDED\"}"), 3);
  EXPECT_EQ(Sample(out, "kestrel_jobs{state=\"RUNNING\"}"), 1);
  EXPECT_EQ(Sample(out, "kestrel_pending_jobs"), 1);
  EXPECT_EQ(Sample(out, "kestrel_workers{status=\"alive\"}"), 1);
  EXPECT_EQ(Sample(out, "kestrel_workers{status=\"dead\"}"), 0);
  EXPECT_NEAR(Sample(out, "kestrel_cluster_cpu_allocation_ratio"), 1000.0 / 4000.0, 1e-6);
  EXPECT_NEAR(Sample(out, "kestrel_cluster_memory_allocation_ratio"), 2048.0 / 8192.0, 1e-6);
  EXPECT_EQ(Sample(out, "kestrel_jobs_submitted_total"), 5);
  // 4 jobs were placed once; the unplaceable one is not in the latency histogram.
  EXPECT_EQ(Sample(out, "kestrel_scheduling_latency_seconds_count"), 4);
  EXPECT_EQ(Sample(out, "kestrel_scheduling_latency_seconds_bucket{le=\"+Inf\"}"), 4);
  EXPECT_GT(Sample(out, "kestrel_wal_frames_total"), 0);
  EXPECT_GT(Sample(out, "kestrel_wal_append_latency_seconds_count"), 0);
  EXPECT_EQ(Sample(out, "kestrel_wal_poisoned"), 0);
  EXPECT_NE(out.find("# TYPE kestrel_scheduling_latency_seconds histogram"), std::string::npos);

  EXPECT_EQ(HttpGet(port, "/healthz"), "HTTP/1.1 200 OK\nok\n");
  EXPECT_EQ(HttpGet(port, "/nope").substr(0, 22), "HTTP/1.1 404 Not Found");

  // Kill the worker: metrics reflect DEAD and the reschedule counters move.
  h.agents["w1"]->Stop();
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf("w1") == WorkerStatus::DEAD; }));
  out = HttpGet(port, "/metrics");
  EXPECT_EQ(Sample(out, "kestrel_workers{status=\"dead\"}"), 1);
  EXPECT_EQ(Sample(out, "kestrel_jobs_lost_total"), 1);
  EXPECT_EQ(Sample(out, "kestrel_jobs_rescheduled_total"), 1);
  EXPECT_EQ(Sample(out, "kestrel_pending_jobs"), 2);
  EXPECT_EQ(Sample(out, "kestrel_cluster_cpu_allocation_ratio"), 0);  // no alive capacity
}

TEST(Metrics, EndpointSurvivesJunkAndSlowClients) {
  Harness h;
  h.StartControl();
  MetricsServer ms([&] { return RenderMetrics(*h.cp, nullptr); });
  int port = ms.Start("127.0.0.1", 0);
  ASSERT_GT(port, 0);
  {  // a client that connects and sends garbage, then one that sends nothing and hangs up
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    const std::string junk("\x00\xff garbage\r\n\r\n", 14);  // 14 = exact literal length
    ::send(fd, junk.data(), junk.size(), 0);
    ::close(fd);
    int fd2 = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_EQ(::connect(fd2, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    ::close(fd2);
  }
  EXPECT_EQ(HttpGet(port, "/healthz"), "HTTP/1.1 200 OK\nok\n");
  EXPECT_EQ(Sample(HttpGet(port, "/metrics"), "kestrel_pending_jobs"), 0);
}

}  // namespace
