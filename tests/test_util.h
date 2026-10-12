#pragma once

#include <cstdlib>
#include <string>
#include <vector>

#include "core/cluster_state.h"

namespace kestrel::testing_util {

inline Resources R(int64_t cpu, int64_t mem) { return Resources{cpu, mem}; }

inline JobSpec Spec(int64_t cpu, int64_t mem, int max_attempts = 3) {
  JobSpec s;
  s.command = "sleep";
  s.args = {"1"};
  s.request = R(cpu, mem);
  s.max_attempts = max_attempts;
  return s;
}

// Registers a worker and returns its id (aborts the test binary on failure).
inline WorkerId AddWorker(ClusterState& st, const std::string& name, Resources cap,
                          TimeUs now = 0) {
  auto r = st.RegisterWorker(name, cap, {}, {}, now);
  if (!r.ok()) std::abort();
  return r.value().worker;
}

inline JobId SubmitJob(ClusterState& st, JobSpec spec, TimeUs now = 0) {
  auto r = st.Submit(std::move(spec), now);
  if (!r.ok()) std::abort();
  return r.value();
}

// Journal that records everything it is given; can be told to fail.
class RecordingJournal : public Journal {
 public:
  Status Append(std::span<const Record> records) override {
    ++appends;
    if (fail) return Status::Error("injected journal failure");
    log.insert(log.end(), records.begin(), records.end());
    return Status::Ok();
  }
  std::vector<Record> log;
  int appends = 0;
  bool fail = false;
};

}  // namespace kestrel::testing_util
