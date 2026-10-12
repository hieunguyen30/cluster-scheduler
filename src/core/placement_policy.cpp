#include "core/placement_policy.h"

namespace kestrel {

std::optional<WorkerId> RoundRobinPolicy::Select(const Resources& request,
                                                 std::span<const Worker> workers) {
  const size_t n = workers.size();
  if (n == 0) return std::nullopt;
  for (size_t i = 0; i < n; ++i) {
    size_t idx = (cursor_ + i) % n;
    if (CanPlace(workers[idx], request)) {
      cursor_ = (idx + 1) % n;
      return workers[idx].id;
    }
  }
  return std::nullopt;  // cursor unchanged: nothing was placed
}

std::optional<WorkerId> FirstFitPolicy::Select(const Resources& request,
                                               std::span<const Worker> workers) {
  for (const Worker& w : workers)
    if (CanPlace(w, request)) return w.id;
  return std::nullopt;
}

double BestFitPolicy::Score(const Worker& w, const Resources& request) {
  Resources left = w.Free() - request;
  return static_cast<double>(left.cpu_millis) / static_cast<double>(w.capacity.cpu_millis) +
         static_cast<double>(left.mem_mib) / static_cast<double>(w.capacity.mem_mib);
}

std::optional<WorkerId> BestFitPolicy::Select(const Resources& request,
                                              std::span<const Worker> workers) {
  const Worker* best = nullptr;
  double best_score = 0;
  for (const Worker& w : workers) {
    if (!CanPlace(w, request)) continue;  // fit check first: only feasible workers are scored
    double s = Score(w, request);
    // Strict '<' keeps the earliest candidate on ties; workers are in id order.
    if (best == nullptr || s < best_score) {
      best = &w;
      best_score = s;
    }
  }
  if (best == nullptr) return std::nullopt;
  return best->id;
}

std::unique_ptr<PlacementPolicy> MakePolicy(std::string_view name) {
  if (name == "round-robin" || name == "rr") return std::make_unique<RoundRobinPolicy>();
  if (name == "first-fit" || name == "ff") return std::make_unique<FirstFitPolicy>();
  if (name == "best-fit" || name == "bf") return std::make_unique<BestFitPolicy>();
  return nullptr;
}

}  // namespace kestrel
