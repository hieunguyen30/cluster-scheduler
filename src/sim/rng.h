// Seeded PRNG for reproducible workloads.
//
// std::mt19937_64 is fully specified by the C++ standard, so the raw stream is identical on
// every conforming toolchain. The standard *distributions* (uniform_int_distribution etc.) are
// NOT specified, so we implement sampling ourselves with integer-only arithmetic.
#pragma once

#include <cstdint>
#include <random>
#include <span>

namespace kestrel::sim {

class Rng {
 public:
  explicit Rng(uint64_t seed) : eng_(seed) {}

  uint64_t Next() { return eng_(); }

  // Uniform integer in [lo, hi], unbiased (rejection sampling).
  uint64_t Uniform(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return lo;
    const uint64_t range = hi - lo + 1;  // may wrap to 0 for the full 64-bit range
    if (range == 0) return Next();
    const uint64_t limit = UINT64_MAX - (UINT64_MAX % range + 1) % range;
    uint64_t r;
    do {
      r = Next();
    } while (r > limit);
    return lo + r % range;
  }

  // Index chosen with probability weights[i] / sum(weights).
  size_t Weighted(std::span<const uint32_t> weights) {
    uint64_t total = 0;
    for (uint32_t w : weights) total += w;
    uint64_t x = Uniform(0, total - 1);
    for (size_t i = 0; i < weights.size(); ++i) {
      if (x < weights[i]) return i;
      x -= weights[i];
    }
    return weights.size() - 1;
  }

 private:
  std::mt19937_64 eng_;
};

}  // namespace kestrel::sim
