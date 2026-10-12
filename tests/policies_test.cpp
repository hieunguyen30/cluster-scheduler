#include <gtest/gtest.h>

#include "core/placement_policy.h"
#include "test_util.h"

using namespace kestrel;
using kestrel::testing_util::R;

namespace {

Worker W(WorkerId id, Resources cap, Resources alloc = {}, WorkerStatus st = WorkerStatus::ALIVE) {
  Worker w;
  w.id = id;
  w.capacity = cap;
  w.allocated = alloc;
  w.status = st;
  return w;
}

TEST(FirstFit, PicksFirstWorkerThatFits) {
  std::vector<Worker> ws = {W(1, R(1000, 1000), R(900, 0)), W(2, R(1000, 1000)),
                            W(3, R(1000, 1000))};
  FirstFitPolicy p;
  EXPECT_EQ(p.Select(R(500, 500), ws), 2u);  // worker 1 has only 100m free
}

TEST(FirstFit, NoneWhenNothingFits) {
  std::vector<Worker> ws = {W(1, R(1000, 1000)), W(2, R(1000, 1000))};
  FirstFitPolicy p;
  EXPECT_FALSE(p.Select(R(2000, 100), ws).has_value());
  EXPECT_FALSE(p.Select(R(100, 2000), ws).has_value());  // memory is checked too
}

TEST(FirstFit, NoWorkers) {
  FirstFitPolicy p;
  EXPECT_FALSE(p.Select(R(1, 1), {}).has_value());
}

TEST(AllPolicies, SkipSuspectAndDeadWorkers) {
  std::vector<Worker> ws = {W(1, R(1000, 1000), {}, WorkerStatus::SUSPECT),
                            W(2, R(1000, 1000), {}, WorkerStatus::DEAD), W(3, R(1000, 1000))};
  for (auto name : {"round-robin", "first-fit", "best-fit"}) {
    auto p = MakePolicy(name);
    EXPECT_EQ(p->Select(R(100, 100), ws), 3u) << name;
    ws[2].status = WorkerStatus::SUSPECT;
    EXPECT_FALSE(p->Select(R(100, 100), ws).has_value()) << name;
    ws[2].status = WorkerStatus::ALIVE;
  }
}

TEST(BestFit, PicksTightestFit) {
  // Both fit; worker 2 is left with less free capacity.
  std::vector<Worker> ws = {W(1, R(4000, 4000)), W(2, R(4000, 4000), R(2500, 2500))};
  BestFitPolicy p;
  EXPECT_EQ(p.Select(R(1000, 1000), ws), 2u);
  FirstFitPolicy ff;
  EXPECT_EQ(ff.Select(R(1000, 1000), ws), 1u);  // contrast with first-fit
}

TEST(BestFit, ChecksFitBeforeScoring) {
  // Worker 1 would have the best (lowest, even negative) score but the job does not fit.
  std::vector<Worker> ws = {W(1, R(1000, 1000), R(900, 900)), W(2, R(4000, 4000))};
  BestFitPolicy p;
  EXPECT_EQ(p.Select(R(500, 500), ws), 2u);
}

TEST(BestFit, NormalisesByEachWorkersCapacity) {
  // Job 1000/1000. Worker 1 (2000/2000, empty) is left 1000/1000 = 50% free per dimension.
  // Worker 2 (100000/100000, 97000 used) is left 2000/2000 = 2% free. Raw leftover favours
  // worker 1 (2000 < 4000); normalised leftover favours worker 2, the genuinely tighter fit.
  std::vector<Worker> ws = {W(1, R(2000, 2000)), W(2, R(100000, 100000), R(97000, 97000))};
  BestFitPolicy p;
  EXPECT_EQ(p.Select(R(1000, 1000), ws), 2u);
  EXPECT_NEAR(BestFitPolicy::Score(ws[0], R(1000, 1000)), 1.0, 1e-9);
  EXPECT_NEAR(BestFitPolicy::Score(ws[1], R(1000, 1000)), 0.04, 1e-9);
}

TEST(BestFit, DifferentSizedWorkers) {
  // small (2c/8Gi), large (8c/32Gi). A 1c/4Gi job leaves 50% vs 87.5% free per dimension.
  std::vector<Worker> ws = {W(1, R(8000, 32768)), W(2, R(2000, 8192))};
  BestFitPolicy p;
  EXPECT_EQ(p.Select(R(1000, 4096), ws), 2u);
}

TEST(BestFit, TiesGoToLowestWorkerId) {
  std::vector<Worker> ws = {W(7, R(1000, 1000)), W(8, R(1000, 1000)), W(9, R(1000, 1000))};
  BestFitPolicy p;
  EXPECT_EQ(p.Select(R(100, 100), ws), 7u);
}

TEST(BestFit, ScoreWeighsCpuAndMemoryEqually) {
  // Documented simplification: a worker left with lots of spare memory but no CPU scores the
  // same as the mirror image.
  Worker a = W(1, R(1000, 1000), R(0, 800));
  Worker b = W(2, R(1000, 1000), R(800, 0));
  EXPECT_NEAR(BestFitPolicy::Score(a, R(100, 100)), BestFitPolicy::Score(b, R(100, 100)), 1e-9);
}

TEST(RoundRobin, RotatesAcrossWorkers) {
  std::vector<Worker> ws = {W(1, R(1000, 1000)), W(2, R(1000, 1000)), W(3, R(1000, 1000))};
  RoundRobinPolicy p;
  std::vector<WorkerId> got;
  for (int i = 0; i < 6; ++i) got.push_back(*p.Select(R(10, 10), ws));
  EXPECT_EQ(got, (std::vector<WorkerId>{1, 2, 3, 1, 2, 3}));
}

TEST(RoundRobin, SkipsWorkersThatDoNotFitAndAdvancesPastChoice) {
  std::vector<Worker> ws = {W(1, R(1000, 1000)), W(2, R(1000, 1000), R(950, 0)),
                            W(3, R(1000, 1000))};
  RoundRobinPolicy p;
  EXPECT_EQ(p.Select(R(100, 100), ws), 1u);
  EXPECT_EQ(p.Select(R(100, 100), ws), 3u);  // 2 is full, skipped
  EXPECT_EQ(p.Select(R(100, 100), ws), 1u);  // wraps
}

TEST(RoundRobin, FailedSelectDoesNotMoveCursor) {
  std::vector<Worker> ws = {W(1, R(1000, 1000)), W(2, R(1000, 1000))};
  RoundRobinPolicy p;
  EXPECT_EQ(p.Select(R(10, 10), ws), 1u);
  EXPECT_FALSE(p.Select(R(5000, 10), ws).has_value());
  EXPECT_EQ(p.Select(R(10, 10), ws), 2u);
}

TEST(RoundRobin, HandlesGrowingWorkerList) {
  std::vector<Worker> ws = {W(1, R(1000, 1000))};
  RoundRobinPolicy p;
  EXPECT_EQ(p.Select(R(10, 10), ws), 1u);
  ws.push_back(W(2, R(1000, 1000)));
  // The cursor wrapped to 0 while there was one worker, so worker 1 is next; then 2.
  EXPECT_EQ(p.Select(R(10, 10), ws), 1u);
  EXPECT_EQ(p.Select(R(10, 10), ws), 2u);
}

TEST(MakePolicy, NamesAndAliases) {
  EXPECT_STREQ(MakePolicy("rr")->Name(), "round-robin");
  EXPECT_STREQ(MakePolicy("ff")->Name(), "first-fit");
  EXPECT_STREQ(MakePolicy("best-fit")->Name(), "best-fit");
  EXPECT_EQ(MakePolicy("nope"), nullptr);
}

}  // namespace
