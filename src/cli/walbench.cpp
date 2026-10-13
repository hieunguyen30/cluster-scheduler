// kestrel_walbench: WAL append latency/throughput for each fsync mode, single writer.
//   kestrel_walbench --dir /tmp --records 3000 [--args 4]
// Every record is a realistic SUBMIT (~150 bytes). Numbers depend entirely on the filesystem and
// disk under --dir; the output echoes the directory so it can be recorded with the result.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "common/flags.h"
#include "wal/wal.h"

using namespace kestrel;
using Clock = std::chrono::steady_clock;

namespace {
double Pct(std::vector<double>& v, double p) {
  std::sort(v.begin(), v.end());
  size_t i = static_cast<size_t>(p * static_cast<double>(v.size()));
  return v[std::min(i, v.size() - 1)];
}
}  // namespace

int main(int argc, char** argv) {
  Flags f(argc, argv);
  const std::string dir = f.Str("dir", "/tmp");
  const int n = static_cast<int>(f.Int("records", 3000));
  const int batch = static_cast<int>(f.Int("batch", 1));
  std::printf("# walbench dir=%s records=%d batch=%d\n", dir.c_str(), n, batch);
  std::printf("%-9s %10s %9s %9s %9s %9s %9s\n", "sync", "ops/s", "p50_us", "p90_us", "p99_us",
              "max_us", "MB/s");
  for (auto [name, mode] : {std::pair{"every", WalSync::EVERY}, std::pair{"interval", WalSync::INTERVAL},
                            std::pair{"none", WalSync::NONE}}) {
    const std::string path = dir + "/kestrel_walbench.wal";
    std::filesystem::remove(path);
    std::vector<Record> recovered;
    WalOptions o;
    o.sync = mode;
    o.interval_ms = 10;
    auto w = Wal::Open(path, o, &recovered);
    if (!w.ok()) {
      std::fprintf(stderr, "open: %s\n", w.status().message().c_str());
      return 1;
    }
    Record r;
    r.type = RecordType::SUBMIT;
    r.spec.command = "sleep";
    r.spec.args = {"1.5"};
    r.spec.request = {500, 256};
    r.spec.max_attempts = 3;
    r.spec.request_id = "bench-request-id-0000";
    std::vector<double> lat;
    auto t0 = Clock::now();
    for (int i = 0; i < n; ++i) {
      r.job = static_cast<JobId>(i + 1);
      std::vector<Record> b(static_cast<size_t>(batch), r);
      auto a = Clock::now();
      if (!w.value()->Append(b).ok()) return 1;
      lat.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
    }
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    // Computed into locals first: function-argument evaluation order is unspecified, and Pct
    // sorts `lat` in place.
    const double mx = *std::max_element(lat.begin(), lat.end());
    const double p50 = Pct(lat, 0.5), p90 = Pct(lat, 0.9), p99 = Pct(lat, 0.99);
    std::printf("%-9s %10.0f %9.0f %9.0f %9.0f %9.0f %9.2f\n", name, n / secs, p50, p90, p99, mx,
                static_cast<double>(w.value()->bytes_written()) / 1e6 / secs);
    w.value().reset();
    std::filesystem::remove(path);
  }
  return 0;
}
