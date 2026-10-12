// Basic value types shared by every Kestrel component.
#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace kestrel {

using JobId = uint64_t;     // 0 is "none"
using WorkerId = uint64_t;  // 0 is "none"
using Epoch = uint64_t;     // 0 is "none"; globally unique and increasing per registration
using TimeUs = int64_t;     // integer microseconds; virtual in the simulator, wall clock in prod

constexpr TimeUs kUnsetTime = -1;
constexpr TimeUs kMillisecond = 1000;
constexpr TimeUs kSecond = 1000 * kMillisecond;

struct Resources {
  int64_t cpu_millis = 0;
  int64_t mem_mib = 0;

  // True if a request of *this size fits inside `free`.
  bool FitsIn(const Resources& free) const {
    return cpu_millis <= free.cpu_millis && mem_mib <= free.mem_mib;
  }
  Resources operator+(const Resources& o) const {
    return {cpu_millis + o.cpu_millis, mem_mib + o.mem_mib};
  }
  Resources operator-(const Resources& o) const {
    return {cpu_millis - o.cpu_millis, mem_mib - o.mem_mib};
  }
  Resources& operator+=(const Resources& o) {
    cpu_millis += o.cpu_millis;
    mem_mib += o.mem_mib;
    return *this;
  }
  Resources& operator-=(const Resources& o) {
    cpu_millis -= o.cpu_millis;
    mem_mib -= o.mem_mib;
    return *this;
  }
  bool operator==(const Resources& o) const {
    return cpu_millis == o.cpu_millis && mem_mib == o.mem_mib;
  }
  bool operator!=(const Resources& o) const { return !(*this == o); }
};

enum class JobState : uint8_t {
  PENDING = 0,
  SCHEDULED,  // assigned to a worker, not yet confirmed running
  RUNNING,
  SUCCEEDED,
  FAILED,
  LOST,  // transient: worker died; immediately becomes PENDING or FAILED
  CANCELLED,
};

enum class WorkerStatus : uint8_t { ALIVE = 0, SUSPECT, DEAD };

inline const char* ToString(JobState s) {
  switch (s) {
    case JobState::PENDING: return "PENDING";
    case JobState::SCHEDULED: return "SCHEDULED";
    case JobState::RUNNING: return "RUNNING";
    case JobState::SUCCEEDED: return "SUCCEEDED";
    case JobState::FAILED: return "FAILED";
    case JobState::LOST: return "LOST";
    case JobState::CANCELLED: return "CANCELLED";
  }
  return "?";
}

inline const char* ToString(WorkerStatus s) {
  switch (s) {
    case WorkerStatus::ALIVE: return "ALIVE";
    case WorkerStatus::SUSPECT: return "SUSPECT";
    case WorkerStatus::DEAD: return "DEAD";
  }
  return "?";
}

inline bool IsTerminal(JobState s) {
  return s == JobState::SUCCEEDED || s == JobState::FAILED || s == JobState::CANCELLED;
}

// Minimal error type: the core avoids exceptions on expected failures.
class Status {
 public:
  Status() = default;  // ok
  static Status Ok() { return Status(); }
  static Status Error(std::string msg) { return Status(std::move(msg)); }
  bool ok() const { return ok_; }
  const std::string& message() const { return msg_; }

 private:
  explicit Status(std::string m) : ok_(false), msg_(std::move(m)) {}
  bool ok_ = true;
  std::string msg_;
};

template <typename T>
class Result {
 public:
  Result(T v) : value_(std::move(v)) {}                       // NOLINT: implicit by design
  Result(Status s) : status_(std::move(s)) {}                 // NOLINT
  bool ok() const { return status_.ok(); }
  const Status& status() const { return status_; }
  const T& value() const { return value_; }
  T& value() { return value_; }

 private:
  Status status_;
  T value_{};
};

}  // namespace kestrel
