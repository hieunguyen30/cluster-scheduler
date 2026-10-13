// Thread-safe fixed-bucket histogram (Prometheus-style cumulative buckets).
#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace kestrel {

class Histogram {
 public:
  // `bounds` are upper bounds in ascending order; an implicit +Inf bucket follows.
  explicit Histogram(std::vector<double> bounds) : bounds_(std::move(bounds)), counts_(bounds_.size() + 1) {}

  // Latency buckets in seconds: 50 us .. 10 s.
  static std::vector<double> LatencyBounds() {
    return {50e-6, 100e-6, 250e-6, 500e-6, 1e-3, 2.5e-3, 5e-3, 10e-3, 25e-3, 50e-3,
            100e-3, 250e-3, 500e-3, 1.0, 2.5, 5.0, 10.0};
  }

  void Observe(double v) {
    std::lock_guard<std::mutex> lk(mu_);
    size_t i = 0;
    while (i < bounds_.size() && v > bounds_[i]) ++i;
    ++counts_[i];
    sum_ += v;
    ++count_;
    if (v > max_) max_ = v;
  }

  struct Snapshot {
    std::vector<double> bounds;
    std::vector<uint64_t> cumulative;  // size = bounds.size() + 1; last entry == count
    double sum = 0;
    uint64_t count = 0;
    double max = 0;
  };

  Snapshot Take() const {
    std::lock_guard<std::mutex> lk(mu_);
    Snapshot s;
    s.bounds = bounds_;
    uint64_t run = 0;
    for (uint64_t c : counts_) {
      run += c;
      s.cumulative.push_back(run);
    }
    s.sum = sum_;
    s.count = count_;
    s.max = max_;
    return s;
  }

 private:
  mutable std::mutex mu_;
  std::vector<double> bounds_;
  std::vector<uint64_t> counts_;
  double sum_ = 0;
  uint64_t count_ = 0;
  double max_ = 0;
};

}  // namespace kestrel
