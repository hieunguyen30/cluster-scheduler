// Failure-handling tests: kill workers, partition workers, fence zombies.
// Timings: heartbeat 100 ms, SUSPECT after 500 ms, DEAD after 1000 ms (see HarnessOptions).
#include <gtest/gtest.h>
#include <signal.h>

#include <fstream>
#include <sstream>

#include "cluster_harness.h"

using namespace kestrel;
using namespace kestrel::testing_util;
using std::chrono::milliseconds;
using std::chrono::seconds;
using Clock = std::chrono::steady_clock;

namespace {

std::string Tmp(const std::string& name) {
  std::string p = ::testing::TempDir() + "kestrel_" + name + "_" + std::to_string(::getpid());
  std::remove(p.c_str());
  return p;
}

std::vector<long> ReadPids(const std::string& path) {
  std::vector<long> pids;
  std::ifstream f(path);
  long p;
  while (f >> p) pids.push_back(p);
  return pids;
}

bool Alive(long pid) { return ::kill(static_cast<pid_t>(pid), 0) == 0; }

// Job body used by the "dies with its worker, succeeds on retry" tests: the first attempt blocks,
// any later attempt sees the marker and exits 0.
std::string FirstAttemptHangs(const std::string& marker) {
  return "if [ -e " + marker + " ]; then exit 0; else touch " + marker + "; sleep 60; fi";
}

TEST(Failure, KilledWorkersJobIsRescheduledAndCompletesElsewhere) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  h.AddWorker("w2", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(2));
  std::string marker = Tmp("marker1");
  JobId j = h.Sh(FirstAttemptHangs(marker), 800, 64, /*attempts=*/3);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  const std::string victim = h.WorkerNameOf(j);
  ASSERT_FALSE(victim.empty());

  auto t_kill = Clock::now();
  h.agents[victim]->Stop();  // the node disappears: jobs die, heartbeats stop
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf(victim) == WorkerStatus::DEAD; }, seconds(15)));
  double detect_s = std::chrono::duration<double>(Clock::now() - t_kill).count();
  std::printf("[measured] kill -> DEAD: %.3f s (dead_after=%.1f s, heartbeat=0.1 s)\n", detect_s,
              TimeScale());
  // Last heartbeat was up to one interval before the kill; the detector polls every 50 ms.
  EXPECT_GE(detect_s, 0.8 * TimeScale());
  EXPECT_LE(detect_s, 4.0 * TimeScale());

  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, seconds(20)));
  Job job = h.Get(j);
  EXPECT_EQ(job.attempts, 2);
  EXPECT_EQ(job.generation, 2u);
  EXPECT_NE(h.WorkerNameOf(j), victim);
  EXPECT_EQ(h.cp->Snapshot().counters.jobs_rescheduled, 1u);
  EXPECT_EQ(h.CheckInvariants(), "");
}

TEST(Failure, JobFailsWhenAttemptsAreExhausted) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  h.AddWorker("w2");
  ASSERT_TRUE(h.WaitForWorkers(2));
  JobId j = h.Sh("sleep 60", 100, 64, /*attempts=*/1);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  h.agents[h.WorkerNameOf(j)]->Stop();
  ASSERT_TRUE(h.WaitState(j, JobState::FAILED, seconds(15)));
  EXPECT_EQ(h.Get(j).attempts, 1);
  EXPECT_EQ(h.cp->Snapshot().counters.jobs_rescheduled, 0u);
}

TEST(Failure, SuspectWorkerGetsNoNewJobsAndRecoversWithoutReregistering) {
  HarnessOptions o;
  o.suspect_after = Scaled(400 * kMillisecond);
  o.dead_after = 30 * kSecond;  // never DEAD in this test
  Harness h(o);
  h.StartControl();
  h.AddWorker("slow", {4000, 8192});
  ASSERT_TRUE(h.WaitForWorkers(1));
  const Epoch epoch_before = h.EpochOf("slow");
  h.agents["slow"]->SetPartitioned(true);  // looks like a long GC pause / network blip
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf("slow") == WorkerStatus::SUSPECT; }, seconds(10)));

  JobId j = h.Sh("true");
  std::this_thread::sleep_for(milliseconds(400));
  EXPECT_EQ(h.Get(j).state, JobState::PENDING);  // no placements on a SUSPECT worker

  h.agents["slow"]->SetPartitioned(false);
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf("slow") == WorkerStatus::ALIVE; }, seconds(10)));
  EXPECT_EQ(h.EpochOf("slow"), epoch_before);  // same incarnation: it was only slow
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED));
  EXPECT_EQ(h.cp->Snapshot().counters.jobs_lost, 0u);
}

// The scenario: the control plane declares a worker dead, but the worker
// is only partitioned and keeps running its job. The job runs twice (at-least-once). When the
// partition heals, fencing must stop the zombie.
TEST(Failure, PartitionedWorkerRunsJobTwiceThenIsFenced) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  h.AddWorker("w2", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(2));
  std::string pidfile = Tmp("pids");
  JobId j = h.Sh("echo $$ >> " + pidfile + "; sleep 60", 800, 64, 3);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  const std::string zombie_worker = h.WorkerNameOf(j);
  const std::string other = zombie_worker == "w1" ? "w2" : "w1";
  ASSERT_TRUE(WaitFor([&] { return ReadPids(pidfile).size() == 1; }));
  const long zombie_pid = ReadPids(pidfile)[0];
  const Epoch old_epoch = h.EpochOf(zombie_worker);

  h.agents[zombie_worker]->SetPartitioned(true);
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf(zombie_worker) == WorkerStatus::DEAD; }, seconds(15)));
  // Rescheduled onto the other worker: now the job runs in two places.
  ASSERT_TRUE(WaitFor([&] { return ReadPids(pidfile).size() == 2; }, seconds(15)));
  EXPECT_EQ(h.WorkerNameOf(j), other);
  EXPECT_EQ(h.Get(j).generation, 2u);
  EXPECT_TRUE(Alive(zombie_pid));  // duplicate execution: at-least-once, as documented

  h.agents[zombie_worker]->SetPartitioned(false);  // heal
  // The zombie's heartbeat is rejected (DEAD / stale epoch), it re-registers reporting job j
  // generation 1, and the control plane tells it to kill it.
  ASSERT_TRUE(WaitFor([&] { return !Alive(zombie_pid); }, seconds(15)));
  EXPECT_GT(h.EpochOf(zombie_worker), old_epoch);
  EXPECT_TRUE(WaitFor([&] { return h.StatusOf(zombie_worker) == WorkerStatus::ALIVE; }));
  // The legitimate attempt is untouched (it may not have been reported RUNNING yet).
  EXPECT_TRUE(h.Get(j).state == JobState::RUNNING || h.Get(j).state == JobState::SCHEDULED);
  EXPECT_EQ(h.WorkerNameOf(j), other);
  EXPECT_TRUE(Alive(ReadPids(pidfile)[1]));
  EXPECT_EQ(h.CheckInvariants(), "");
  // Clean up the long-running job.
  v1::CancelJobRequest req;
  req.set_job_id(j);
  v1::CancelJobResponse resp;
  grpc::ClientContext ctx;
  ASSERT_TRUE(h.stub->CancelJob(&ctx, req, &resp).ok());
}

// A zombie that FINISHES while partitioned must not complete the job: its result belongs to a
// superseded attempt (assignment_generation fence).
TEST(Failure, ResultFromSupersededAttemptIsIgnored) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  h.AddWorker("w2", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(2));
  std::string counter = Tmp("runs");
  // 1st execution: 2 s then exit 0. 2nd execution: 5 s then exit 0.
  JobId j = h.Sh("n=$(wc -l < " + counter + " 2>/dev/null || echo 0); echo x >> " + counter +
                     "; if [ \"$n\" = 0 ]; then sleep " + ScaledSeconds(2) + "; else sleep " + ScaledSeconds(5) + "; fi",
                 800, 64, 3);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  const std::string first = h.WorkerNameOf(j);
  h.agents[first]->SetPartitioned(true);  // its attempt will finish at ~2 s, unseen
  ASSERT_TRUE(WaitFor([&] { return h.StatusOf(first) == WorkerStatus::DEAD; }, seconds(15)));
  ASSERT_TRUE(WaitFor([&] { return h.Get(j).generation == 2 && h.Get(j).state != JobState::PENDING; }));
  // Let the zombie finish (its result is queued on the worker), then heal while attempt 2 runs.
  ASSERT_TRUE(WaitFor([&] { return h.agents[first]->RunningCount() == 0; }, seconds(10)));
  ASSERT_EQ(h.Get(j).state == JobState::SUCCEEDED, false);
  h.agents[first]->SetPartitioned(false);
  ASSERT_TRUE(WaitFor([&] { return h.cp->Snapshot().counters.stale_reports_ignored >= 1; },
                      seconds(10)));
  EXPECT_NE(h.Get(j).state, JobState::SUCCEEDED);  // the stale result did not complete it
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, seconds(20)));
  EXPECT_EQ(h.Get(j).generation, 2u);
  EXPECT_EQ(h.Get(j).attempts, 2);
  EXPECT_EQ(h.CheckInvariants(), "");
}

TEST(Failure, RestartedWorkerWithSameNameLosesItsOldJobs) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  std::string marker = Tmp("marker2");
  JobId j = h.Sh(FirstAttemptHangs(marker), 100, 64, 3);
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  const Epoch e1 = h.EpochOf("w1");
  h.agents["w1"]->Stop();                                  // pod crashes...
  std::this_thread::sleep_for(milliseconds(150));
  h.AddWorker("w1");                                       // ...and restarts under the same name,
                                                           // well inside the dead timeout.
  ASSERT_TRUE(WaitFor([&] { return h.EpochOf("w1") > e1; }));
  // Its report is empty, so the old assignment is LOST at registration (not after 1 s).
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, seconds(20)));
  EXPECT_EQ(h.Get(j).attempts, 2);
  EXPECT_EQ(h.cp->ListWorkers().size(), 1u);  // same identity, new incarnation
}

TEST(Failure, EveryJobFinishesDespiteMultipleWorkerKills) {
  Harness h;
  h.StartControl();
  for (int i = 0; i < 4; ++i) h.AddWorker("w" + std::to_string(i), {2000, 8192});
  ASSERT_TRUE(h.WaitForWorkers(4));
  std::vector<JobId> ids;
  for (int i = 0; i < 24; ++i) ids.push_back(h.Sh("sleep 0.4", 500, 128, /*attempts=*/10));
  ASSERT_TRUE(WaitFor([&] {
    size_t running = 0;
    for (JobId id : ids) running += h.Get(id).state == JobState::RUNNING;
    return running >= 4;
  }));
  h.agents["w0"]->Stop();
  h.agents["w1"]->Stop();
  for (JobId id : ids) ASSERT_TRUE(h.WaitState(id, JobState::SUCCEEDED, seconds(60))) << "job " << id;
  size_t retried = 0;
  for (JobId id : ids) retried += h.Get(id).attempts > 1;
  EXPECT_GE(retried, 1u);  // some jobs were on the killed workers
  EXPECT_EQ(h.CheckInvariants(), "");
  for (const Worker& w : h.cp->ListWorkers()) EXPECT_EQ(w.allocated, (Resources{0, 0})) << w.name;
  std::printf("[measured] 24 jobs, 2 of 4 workers killed: %zu jobs retried, %llu LOST attempts\n",
              retried, static_cast<unsigned long long>(h.cp->Snapshot().counters.jobs_lost));
}

TEST(Failure, ResultFinishedDuringBriefPartitionIsDeliveredNotRerun) {
  // The worker goes SUSPECT (never DEAD), finishes its job while unreachable, and delivers the
  // result once the partition heals. The job must not run a second time.
  HarnessOptions o;
  o.suspect_after = Scaled(300 * kMillisecond);
  o.dead_after = 30 * kSecond;
  Harness h(o);
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  std::string counter = Tmp("runs2");
  JobId j = h.Sh("echo x >> " + counter + "; sleep 0.5");
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  h.agents["w1"]->SetPartitioned(true);
  ASSERT_TRUE(WaitFor([&] { return h.agents["w1"]->RunningCount() == 0; }, seconds(10)));
  EXPECT_EQ(h.Get(j).state, JobState::RUNNING);  // control plane has not heard about it
  h.agents["w1"]->SetPartitioned(false);
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED));
  EXPECT_EQ(h.Get(j).attempts, 1);
  std::ifstream f(counter);
  std::string line;
  int runs = 0;
  while (std::getline(f, line)) ++runs;
  EXPECT_EQ(runs, 1);
}

}  // namespace
