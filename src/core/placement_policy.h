// Placement policies. A policy answers one question: given a job's
// request and the current workers, which worker (if any) should run it?
#pragma once

#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "core/model.h"

namespace kestrel {

class PlacementPolicy {
 public:
  virtual ~PlacementPolicy() = default;
  virtual const char* Name() const = 0;
  // Returns the chosen worker, or nullopt if no ALIVE worker has room. Policies never mutate
  // cluster state. Only ALIVE workers are candidates (SUSPECT workers get no new placements).
  virtual std::optional<WorkerId> Select(const Resources& request,
                                         std::span<const Worker> workers) = 0;
};

// True if `w` may receive a job of this size right now.
inline bool CanPlace(const Worker& w, const Resources& request) {
  return w.status == WorkerStatus::ALIVE && request.FitsIn(w.Free());
}

// Baseline. Rotating start cursor; takes the first worker at or after the cursor that fits.
class RoundRobinPolicy : public PlacementPolicy {
 public:
  const char* Name() const override { return "round-robin"; }
  std::optional<WorkerId> Select(const Resources& request,
                                 std::span<const Worker> workers) override;

 private:
  size_t cursor_ = 0;
};

// First worker (in registration order) with enough free CPU and memory.
class FirstFitPolicy : public PlacementPolicy {
 public:
  const char* Name() const override { return "first-fit"; }
  std::optional<WorkerId> Select(const Resources& request,
                                 std::span<const Worker> workers) override;
};

// Among workers that fit, the one left with the least free capacity after placement.
// Score = leftover_cpu/capacity_cpu + leftover_mem/capacity_mem, each normalised by that
// worker's own capacity so differently sized workers are comparable. Equal CPU/memory weight
// is a simplification. Ties go to the lowest worker id.
class BestFitPolicy : public PlacementPolicy {
 public:
  const char* Name() const override { return "best-fit"; }
  std::optional<WorkerId> Select(const Resources& request,
                                 std::span<const Worker> workers) override;
  static double Score(const Worker& w, const Resources& request);
};

// "round-robin" | "first-fit" | "best-fit" (also rr / ff / bf). nullptr if unknown.
std::unique_ptr<PlacementPolicy> MakePolicy(std::string_view name);

}  // namespace kestrel
