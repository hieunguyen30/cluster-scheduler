// Control-plane restart tests (DESIGN.md section 6 / Experiment C): the control plane is killed
// mid-run and rebuilt from its WAL while worker agents keep running their jobs.
#include <gtest/gtest.h>
#include <unistd.h>

#include <fstream>
#include <map>

#include "cluster_harness.h"

using namespace kestrel;
using namespace kestrel::testing_util;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Clock = std::chrono::steady_clock;

namespace {

std::string Tmp(const std::string& name) {
  static int n = 0;
  std::string p = ::testing::TempDir() + "kestrel_rs_" + name + "_" + std::to_string(::getpid()) +
                  "_" + std::to_string(n++);
  std::remove(p.c_str());
  return p;
}

std::vector<std::string> Lines(const std::string& path) {
  std::vector<std::string> out;
  std::ifstream f(path);
  std::string l;
  while (std::getline(f, l)) out.push_back(l);
  return out;
}

bool AllWorkersAliveWithNewEpoch(Harness& h, const std::map<std::string, Epoch>& old) {
  for (const auto& [name, e] : old)
    if (h.StatusOf(name) != WorkerStatus::ALIVE || h.EpochOf(name) <= e) return false;
  return true;
}

TEST(Restart, JobsSurviveAndRunningWorkIsAdoptedNotRerun) {
  Harness h({}, Tmp("wal1"));
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  h.AddWorker("w2", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(2));
  std::string runs = Tmp("runs1");
  JobId done = h.Sh("true", 100, 64);
  ASSERT_TRUE(h.WaitState(done, JobState::SUCCEEDED));
  JobId running = h.Sh("echo A >> " + runs + "; sleep " + ScaledSeconds(3), 800, 64);
  ASSERT_TRUE(h.WaitState(running, JobState::RUNNING));
  JobId waiting = h.Sh("true", 5000, 64);  // fits no worker: stays PENDING across the restart
  std::this_thread::sleep_for(milliseconds(200));
  ASSERT_EQ(h.Get(waiting).state, JobState::PENDING);
  const std::string where = h.WorkerNameOf(running);
  std::map<std::string, Epoch> old_epochs = {{"w1", h.EpochOf("w1")}, {"w2", h.EpochOf("w2")}};

  auto t0 = Clock::now();
  h.RestartControl();

  // Immediately after recovery: durable state is back, worker liveness is not yet known.
  EXPECT_EQ(h.Get(done).state, JobState::SUCCEEDED);
  EXPECT_EQ(h.Get(waiting).state, JobState::PENDING);
  Job r = h.Get(running);
  EXPECT_TRUE(r.state == JobState::SCHEDULED || r.state == JobState::RUNNING);
  EXPECT_EQ(r.attempts, 1);
  EXPECT_EQ(h.WorkerNameOf(running), where);

  ASSERT_TRUE(WaitFor([&] { return AllWorkersAliveWithNewEpoch(h, old_epochs); }, seconds(20)));
  double recovered_s = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("[measured] control-plane restart -> all workers re-registered: %.3f s "
              "(heartbeat 0.1 s)\n", recovered_s);

  ASSERT_TRUE(h.WaitState(running, JobState::SUCCEEDED, seconds(30)));
  EXPECT_EQ(h.Get(running).attempts, 1);           // adopted, not rescheduled
  EXPECT_EQ(Lines(runs).size(), 1u);               // and it really ran only once
  EXPECT_EQ(h.cp->Snapshot().counters.jobs_lost, 0u);
  JobId fresh = h.Sh("true");
  EXPECT_GT(fresh, waiting);                       // ids continue past the replayed ones
  EXPECT_EQ(h.CheckInvariants(), "");
}

TEST(Restart, ResultFinishedWhileControlPlaneWasDownIsDelivered) {
  Harness h({}, Tmp("wal2"));
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  std::string runs = Tmp("runs2");
  JobId j = h.Sh("echo X >> " + runs + "; sleep 0.4");
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  h.CrashControl();
  std::this_thread::sleep_for(milliseconds(1000));  // job exits while nobody is listening
  h.StartControl("127.0.0.1:" + std::to_string(h.port));
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, seconds(20)));
  EXPECT_EQ(h.Get(j).attempts, 1);
  EXPECT_EQ(Lines(runs).size(), 1u);
}

TEST(Restart, WorkerThatNeverComesBackIsDeclaredDeadAndItsJobsRerun) {
  Harness h({}, Tmp("wal3"));
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  h.AddWorker("w2", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(2));
  std::string marker = Tmp("marker3");
  JobId j = h.Sh("if [ -e " + marker + " ]; then exit 0; else touch " + marker + "; sleep 60; fi",
                 800, 64, 3);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  const std::string victim = h.WorkerNameOf(j);
  // The node dies at the same time as the control plane.
  h.agents[victim]->Stop();
  h.RestartControl();
  EXPECT_NE(h.Get(j).state, JobState::PENDING);  // replay still believes it is on the victim
  // The victim is a "ghost" (SUSPECT); after dead_after it is DEAD and the job moves.
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf(victim) == WorkerStatus::DEAD; }, seconds(20)));
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, seconds(30)));
  EXPECT_EQ(h.Get(j).attempts, 2);
  EXPECT_NE(h.WorkerNameOf(j), victim);
}

TEST(Restart, RetriedSubmitWithSameRequestIdAfterRestartIsDeduplicated) {
  Harness h({}, Tmp("wal4"));
  h.StartControl();
  JobId a = h.Submit("true", {}, 100, 64, 0, "op-77");
  h.RestartControl();
  JobId again = h.Submit("true", {}, 100, 64, 0, "op-77");  // client never saw the first reply
  EXPECT_EQ(a, again);
  EXPECT_EQ(h.cp->ListJobs(std::nullopt, 0).size(), 1u);
}

TEST(Restart, TornTailLeftByTheCrashIsTruncatedAndStateIsIntact) {
  std::string wal = Tmp("wal5");
  Harness h({}, wal);
  h.StartControl();
  JobId a = h.Submit("true", {});
  JobId b = h.Submit("true", {});
  h.CrashControl();
  {  // a crash mid-append leaves half a frame at the end
    std::ofstream f(wal, std::ios::binary | std::ios::app);
    const char partial[] = {0x40, 0, 0, 0, 0x12, 0x34, 0x56};
    f.write(partial, sizeof partial);
  }
  h.StartControl("127.0.0.1:" + std::to_string(h.port));
  EXPECT_EQ(h.recovery.torn_bytes_truncated, 7u);
  EXPECT_EQ(h.cp->ListJobs(std::nullopt, 0).size(), 2u);
  EXPECT_EQ(h.Get(a).state, JobState::PENDING);
  EXPECT_EQ(h.Get(b).state, JobState::PENDING);
  JobId c = h.Submit("true", {});
  EXPECT_GT(c, b);
}

// Experiment C in miniature: restart the control plane three times while a batch is running.
// Every job must finish exactly once: none lost, none run twice, none stuck.
TEST(Restart, RepeatedRestartsUnderLoadLoseAndDuplicateNothing) {
  Harness h({}, Tmp("wal6"));
  h.StartControl();
  for (int i = 0; i < 3; ++i) h.AddWorker("w" + std::to_string(i), {2000, 8192});
  ASSERT_TRUE(h.WaitForWorkers(3));
  std::string runs = Tmp("runs6");
  const int kJobs = 40;
  std::vector<JobId> ids;
  for (int i = 0; i < kJobs; ++i)
    ids.push_back(h.Sh("echo " + std::to_string(i) + " >> " + runs + "; sleep 0.3", 500, 128));
  for (int round = 0; round < 3; ++round) {
    std::this_thread::sleep_for(milliseconds(700));
    h.RestartControl();
  }
  for (JobId id : ids) ASSERT_TRUE(h.WaitState(id, JobState::SUCCEEDED, seconds(60))) << "job " << id;
  std::map<std::string, int> counts;
  for (const auto& l : Lines(runs)) ++counts[l];
  EXPECT_EQ(counts.size(), static_cast<size_t>(kJobs));  // none lost
  int dups = 0;
  for (auto& kv : counts) dups += kv.second > 1;
  EXPECT_EQ(dups, 0) << "jobs executed more than once";    // none double-run
  size_t reattempts = 0;
  for (JobId id : ids) reattempts += h.Get(id).attempts > 1;
  std::printf("[measured] 3 control-plane restarts, %d jobs: %d duplicate executions, "
              "%zu jobs with attempts>1\n", kJobs, dups, reattempts);
  EXPECT_EQ(h.CheckInvariants(), "");
  for (const Worker& w : h.cp->ListWorkers()) EXPECT_EQ(w.allocated, (Resources{0, 0})) << w.name;
}

}  // namespace
