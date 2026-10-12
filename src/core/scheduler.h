// One scheduling pass: strict FIFO with head-of-line blocking (DESIGN.md section 4).
#pragma once

#include <memory>
#include <vector>

#include "core/cluster_state.h"
#include "core/placement_policy.h"

namespace kestrel {

class Scheduler {
 public:
  explicit Scheduler(std::unique_ptr<PlacementPolicy> policy) : policy_(std::move(policy)) {}

  // Place jobs from the head of the pending queue until the head fits nowhere (or the queue
  // is empty). Never skips the head, so a large job blocks everything behind it: documented
  // limitation, backfill is a possible extension. Returns the jobs placed, in order.
  std::vector<JobId> RunPass(ClusterState& state, TimeUs now);

  PlacementPolicy& policy() { return *policy_; }

 private:
  std::unique_ptr<PlacementPolicy> policy_;
};

}  // namespace kestrel
