#include <gtest/gtest.h>

#include <map>

#include "sim/rng.h"
#include "sim/simulator.h"
#include "sim/workload.h"

using namespace kestrel;
using namespace kestrel::sim;

namespace {

SimJob J(TimeUs arrival, TimeUs duration, int64_t cpu, int64_t mem) {
  SimJob j;
  j.arrival = arrival;
  j.duration = duration;
  j.spec.command = "sleep";
  j.spec.request = {cpu, mem};
  return j;
}

TEST(Rng, MatchesStandardMt19937_64Stream) {
  // The C++ standard requires the 10000th output of a default-seeded mt19937_64; the first
  // output for the default seed 5489 is a well-known constant.
  Rng r(5489);
  EXPECT_EQ(r.Next(), 14514284786278117030ull);
}

TEST(Rng, UniformStaysInRangeAndCoversIt) {
  Rng r(7);
  std::map<uint64_t, int> seen;
  for (int i = 0; i < 6000; ++i) {
    uint64_t v = r.Uniform(3, 8);
    ASSERT_GE(v, 3u);
    ASSERT_LE(v, 8u);
    ++seen[v];
  }
  EXPECT_EQ(seen.size(), 6u);
  for (auto& kv : seen) EXPECT_NEAR(kv.second, 1000, 150);  // roughly uniform
  EXPECT_EQ(r.Uniform(5, 5), 5u);
}

TEST(Rng, WeightedFollowsWeights) {
  Rng r(11);
  std::vector<uint32_t> w = {1, 0, 3};
  int counts[3] = {};
  for (int i = 0; i < 8000; ++i) ++counts[r.Weighted(w)];
  EXPECT_EQ(counts[1], 0);
  EXPECT_NEAR(counts[0], 2000, 200);
  EXPECT_NEAR(counts[2], 6000, 200);
}

TEST(Workload, DeterministicPerSeedAndDifferentAcrossSeeds) {
  WorkloadConfig c;
  c.num_jobs = 300;
  c.seed = 42;
  auto a = GenerateWorkload(c), b = GenerateWorkload(c);
  ASSERT_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    EXPECT_EQ(a[i].arrival, b[i].arrival);
    EXPECT_EQ(a[i].duration, b[i].duration);
    EXPECT_EQ(a[i].spec.request, b[i].spec.request);
  }
  c.seed = 43;
  auto d = GenerateWorkload(c);
  bool differs = false;
  for (size_t i = 0; i < a.size(); ++i) differs |= a[i].arrival != d[i].arrival;
  EXPECT_TRUE(differs);
}

TEST(Workload, ArrivalsAreNonDecreasingAndDurationsInClassRange) {
  WorkloadConfig c;
  c.num_jobs = 500;
  auto jobs = GenerateWorkload(c);
  for (size_t i = 1; i < jobs.size(); ++i) EXPECT_GE(jobs[i].arrival, jobs[i - 1].arrival);
  for (const SimJob& j : jobs) {
    bool ok = false;
    for (const JobClass& jc : c.classes)
      if (jc.name == j.job_class)
        ok = j.duration >= jc.min_duration && j.duration <= jc.max_duration &&
             j.spec.request == jc.request;
    EXPECT_TRUE(ok) << j.job_class;
  }
}

TEST(Workload, OfferedLoadMatchesTargetWithinTolerance) {
  WorkloadConfig c;
  c.num_jobs = 20000;
  c.target_load = 0.7;
  auto jobs = GenerateWorkload(c);
  double cpu_work = 0, mem_work = 0;
  for (const SimJob& j : jobs) {
    cpu_work += static_cast<double>(j.spec.request.cpu_millis) * static_cast<double>(j.duration);
    mem_work += static_cast<double>(j.spec.request.mem_mib) * static_cast<double>(j.duration);
  }
  double cap_cpu = 0, cap_mem = 0;
  for (auto& r : c.fleet) {
    cap_cpu += static_cast<double>(r.cpu_millis);
    cap_mem += static_cast<double>(r.mem_mib);
  }
  double span = static_cast<double>(jobs.back().arrival);
  double load = std::max(cpu_work / (span * cap_cpu), mem_work / (span * cap_mem));
  EXPECT_NEAR(load, 0.7, 0.05);
}

TEST(Percentile, NearestRank) {
  EXPECT_EQ(Percentile({}, 0.5), 0);
  EXPECT_EQ(Percentile({5}, 0.99), 5);
  EXPECT_EQ(Percentile({10, 20, 30, 40}, 0.50), 20);
  EXPECT_EQ(Percentile({40, 10, 30, 20}, 0.99), 40);
  std::vector<TimeUs> v;
  for (int i = 1; i <= 100; ++i) v.push_back(i);
  EXPECT_EQ(Percentile(v, 0.99), 99);
}

// Hand-computed: one 1000m/1000MiB worker, two jobs of the full worker, 10 s each.
TEST(Simulator, HandComputedSerialJobs) {
  std::vector<Resources> fleet = {{1000, 1000}};
  std::vector<SimJob> jobs = {J(0, 10 * kSecond, 1000, 1000), J(0, 10 * kSecond, 1000, 1000)};
  SimOptions o;
  o.check_invariants = true;
  SimResult r = RunSimulation("first-fit", 0, fleet, jobs, o);
  EXPECT_EQ(r.makespan_us, 20 * kSecond);
  EXPECT_EQ(r.jobs_succeeded, 2u);
  EXPECT_NEAR(r.cpu_util_makespan, 1.0, 1e-9);
  EXPECT_NEAR(r.mem_util_makespan, 1.0, 1e-9);
  EXPECT_NEAR(r.busy_fraction, 0.5, 1e-9);        // queue non-empty for [0,10s)
  EXPECT_NEAR(r.cpu_util_busy, 1.0, 1e-9);
  EXPECT_NEAR(r.frag_fraction_makespan, 0.0, 1e-9);  // waiting for capacity, not fragmented
  EXPECT_EQ(r.wait_p50_us, 0);                    // waits are {0, 10s}; nearest-rank p50 = 0
  EXPECT_EQ(r.wait_p99_us, 10 * kSecond);
  EXPECT_NEAR(r.wait_mean_us, 5e6, 1e-3);
  EXPECT_EQ(r.max_queue_depth, 1u);
}

// Hand-computed fragmentation: two 1000/1000 workers. A=600 and B=600 land on separate workers
// (first-fit: B does not fit next to A). C=800 then fits nowhere although 800 is free in
// aggregate => blocked by fragmentation until A and B finish at t=10s.
TEST(Simulator, HandComputedFragmentation) {
  std::vector<Resources> fleet = {{1000, 1000}, {1000, 1000}};
  std::vector<SimJob> jobs = {J(0, 10 * kSecond, 600, 600), J(0, 10 * kSecond, 600, 600),
                              J(0, 5 * kSecond, 800, 800)};
  SimResult r = RunSimulation("first-fit", 0, fleet, jobs, {true});
  EXPECT_EQ(r.makespan_us, 15 * kSecond);
  EXPECT_NEAR(r.busy_fraction, 10.0 / 15.0, 1e-9);
  EXPECT_NEAR(r.frag_fraction_makespan, 10.0 / 15.0, 1e-9);
  EXPECT_NEAR(r.frag_fraction_busy, 1.0, 1e-9);
  EXPECT_EQ(r.wait_p99_us, 10 * kSecond);
  // Utilization: (600+600)*10 + 800*5 = 16000 + ... in cpu*s over 15s * 2000 capacity.
  EXPECT_NEAR(r.cpu_util_makespan, (1200.0 * 10 + 800.0 * 5) / (15.0 * 2000.0), 1e-9);
  // Over the busy window [0,10s) only A and B ran: 1200/2000.
  EXPECT_NEAR(r.cpu_util_busy, 0.6, 1e-9);
}

TEST(Simulator, SameEventsSameResult) {
  WorkloadConfig c;
  c.num_jobs = 400;
  c.seed = 9;
  auto jobs = GenerateWorkload(c);
  for (const char* p : {"round-robin", "first-fit", "best-fit"}) {
    SimResult a = RunSimulation(p, 9, c.fleet, jobs);
    SimResult b = RunSimulation(p, 9, c.fleet, jobs);
    EXPECT_EQ(a.makespan_us, b.makespan_us) << p;
    EXPECT_EQ(a.wait_p99_us, b.wait_p99_us) << p;
    EXPECT_EQ(a.cpu_util_makespan, b.cpu_util_makespan) << p;  // bit-identical, same binary
    EXPECT_EQ(a.frag_fraction_busy, b.frag_fraction_busy) << p;
  }
}

TEST(Simulator, EveryJobCompletesAndInvariantsHoldForAllPolicies) {
  for (uint64_t seed = 1; seed <= 5; ++seed) {
    WorkloadConfig c;
    c.num_jobs = 600;
    c.seed = seed;
    c.target_load = 0.95;
    c.fleet = MakeFleet(6);
    auto jobs = GenerateWorkload(c);
    for (const char* p : {"round-robin", "first-fit", "best-fit"}) {
      SimOptions o;
      o.check_invariants = true;
      SimResult r = RunSimulation(p, seed, c.fleet, jobs, o);
      EXPECT_EQ(r.jobs_succeeded, jobs.size()) << p << " seed " << seed;
      EXPECT_LE(r.cpu_util_makespan, 1.0 + 1e-9);
      EXPECT_LE(r.mem_util_makespan, 1.0 + 1e-9);
      EXPECT_GE(r.frag_fraction_busy, 0.0);
      EXPECT_LE(r.frag_fraction_busy, 1.0 + 1e-9);
    }
  }
}

TEST(Simulator, SimultaneousArrivalsKeepSubmissionOrder) {
  // One slot; three same-instant arrivals must run in arrival order (seq tie-break).
  std::vector<Resources> fleet = {{100, 100}};
  std::vector<SimJob> jobs = {J(0, 1 * kSecond, 100, 100), J(0, 2 * kSecond, 100, 100),
                              J(0, 3 * kSecond, 100, 100)};
  SimResult r = RunSimulation("first-fit", 0, fleet, jobs);
  EXPECT_EQ(r.makespan_us, 6 * kSecond);
  EXPECT_EQ(r.wait_p99_us, 3 * kSecond);  // third job waits for the first two: 1s + 2s
}

}  // namespace
