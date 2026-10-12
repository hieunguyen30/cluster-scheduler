#include "core/scheduler.h"

namespace kestrel {

std::vector<JobId> Scheduler::RunPass(ClusterState& state, TimeUs now) {
  std::vector<JobId> placed;
  while (auto head = state.PendingHead()) {
    const Job* job = state.FindJob(*head);
    auto choice = policy_->Select(job->spec.request, state.Workers());
    if (!choice) break;  // head-of-line blocking
    if (!state.Assign(*head, *choice, now).ok()) break;  // e.g. journal failure: retry later
    placed.push_back(*head);
  }
  return placed;
}

}  // namespace kestrel
