#include <gtest/gtest.h>

#include "core/scheduler.h"
#include "test_util.h"

using namespace kestrel;
using namespace kestrel::testing_util;

namespace {

TEST(Scheduler, PlacesInFifoOrder) {
  ClusterState st;
  AddWorker(st, "w", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  JobId b = SubmitJob(st, Spec(100, 100));
  JobId c = SubmitJob(st, Spec(100, 100));
  Scheduler s(MakePolicy("first-fit"));
  EXPECT_EQ(s.RunPass(st, 1), (std::vector<JobId>{a, b, c}));
  EXPECT_TRUE(st.Pending().empty());
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Scheduler, HeadOfLineBlockingIsDeliberate) {
  ClusterState st;
  AddWorker(st, "w", R(1000, 1000));
  JobId fill = SubmitJob(st, Spec(700, 700));
  JobId big = SubmitJob(st, Spec(800, 800));    // cannot fit next to `fill`
  JobId small = SubmitJob(st, Spec(100, 100));  // would fit, but is behind `big`
  Scheduler s(MakePolicy("first-fit"));
  EXPECT_EQ(s.RunPass(st, 1), std::vector<JobId>{fill});
  EXPECT_EQ(st.FindJob(small)->state, JobState::PENDING);  // not skipped
  EXPECT_EQ(st.PendingHead(), big);
  // Free capacity: big goes first, then small.
  ASSERT_TRUE(st.ReportFinished(fill, 1, 1, 0, 2).ok());
  EXPECT_EQ(s.RunPass(st, 2), (std::vector<JobId>{big, small}));
}

TEST(Scheduler, NothingToDoWithoutWorkersOrJobs) {
  ClusterState st;
  Scheduler s(MakePolicy("best-fit"));
  EXPECT_TRUE(s.RunPass(st, 0).empty());
  SubmitJob(st, Spec(100, 100));
  EXPECT_TRUE(s.RunPass(st, 0).empty());  // no workers
  EXPECT_EQ(st.Pending().size(), 1u);
}

TEST(Scheduler, RequeuedJobGoesBeforeNewerJobs) {
  ClusterState st;
  WorkerId w1 = AddWorker(st, "w1", R(1000, 1000));
  AddWorker(st, "w2", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  Scheduler s(MakePolicy("first-fit"));
  s.RunPass(st, 1);
  JobId b = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.MarkWorkerDead(w1, 2).ok());
  ASSERT_EQ(st.PendingHead(), a);  // ahead of b
  auto placed = s.RunPass(st, 3);
  EXPECT_EQ(placed, (std::vector<JobId>{a, b}));
  EXPECT_EQ(st.FindJob(a)->assigned_worker, st.FindWorkerByName("w2")->id);
}

size_t PendingAfter(const char* policy, const std::vector<int>& sizes) {
  ClusterState st;
  AddWorker(st, "w1", R(1000, 1000));
  AddWorker(st, "w2", R(1000, 1000));
  for (int s : sizes) SubmitJob(st, Spec(s, s));
  Scheduler sched(MakePolicy(policy));
  sched.RunPass(st, 0);
  return st.Pending().size();
}

// Two 1000/1000 bins, jobs 500, 700, 300, 500 (all fit in aggregate: 2000 total).
//   first-fit / round-robin put the 300 next to the 500, leaving 200 and 300 free on two
//   bins; best-fit puts the 300 into the 700 bin (tightest), keeping 500 free for the last job.
TEST(Scheduler, BestFitBeatsFirstFitAndRoundRobinOnThisInput) {
  EXPECT_EQ(PendingAfter("best-fit", {500, 700, 300, 500}), 0u);
  EXPECT_EQ(PendingAfter("first-fit", {500, 700, 300, 500}), 1u);
  EXPECT_EQ(PendingAfter("round-robin", {500, 700, 300, 500}), 1u);
}

// The failure mode (interview question 1): online best-fit is only a heuristic. With small
// jobs first it packs them together and strands the large ones, while round-robin's spreading
// happens to leave 700 free on each bin.
TEST(Scheduler, BestFitIsNotOptimalOnline) {
  EXPECT_EQ(PendingAfter("best-fit", {300, 300, 700, 700}), 1u);
  EXPECT_EQ(PendingAfter("first-fit", {300, 300, 700, 700}), 1u);
  EXPECT_EQ(PendingAfter("round-robin", {300, 300, 700, 700}), 0u);
}

}  // namespace
