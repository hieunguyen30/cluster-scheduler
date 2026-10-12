#include <gtest/gtest.h>

#include "sim/rng.h"
#include "test_util.h"

using namespace kestrel;
using namespace kestrel::testing_util;

namespace {

constexpr TimeUs kS = kSecond;

TEST(Submit, AssignsSequentialIdsAndQueuesFifo) {
  ClusterState st;
  JobId a = SubmitJob(st, Spec(100, 100), 5);
  JobId b = SubmitJob(st, Spec(100, 100), 6);
  EXPECT_EQ(a, 1u);
  EXPECT_EQ(b, 2u);
  EXPECT_EQ(st.PendingHead(), a);
  EXPECT_EQ(st.FindJob(a)->state, JobState::PENDING);
  EXPECT_EQ(st.FindJob(a)->submitted, 5);
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Submit, RejectsInvalidSpecs) {
  ClusterState st;
  EXPECT_FALSE(st.Submit(Spec(0, 100), 0).ok());
  EXPECT_FALSE(st.Submit(Spec(100, 0), 0).ok());
  EXPECT_FALSE(st.Submit(Spec(-1, 100), 0).ok());
  EXPECT_FALSE(st.Submit(Spec(100, 100, 0), 0).ok());
  EXPECT_EQ(st.JobCount(), 0u);
}

TEST(Submit, RequestIdMakesRetriesIdempotentAcrossReplay) {
  RecordingJournal j;
  ClusterState st(&j);
  JobSpec s = Spec(100, 100);
  s.request_id = "req-1";
  JobId a = SubmitJob(st, s);
  JobId again = SubmitJob(st, s);  // the client retried after a lost reply
  EXPECT_EQ(a, again);
  EXPECT_EQ(st.JobCount(), 1u);
  EXPECT_EQ(j.log.size(), 1u);  // the retry wrote nothing
  s.request_id = "req-2";
  EXPECT_NE(SubmitJob(st, s), a);
  // After a restart the key is rebuilt from the log, so a retry is still deduplicated.
  ClusterState re;
  for (const Record& r : j.log) re.Replay(r);
  s.request_id = "req-1";
  EXPECT_EQ(SubmitJob(re, s), a);
  EXPECT_EQ(re.JobCount(), 2u);
}

TEST(Assign, UpdatesAccountingAndState) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId j = SubmitJob(st, Spec(400, 300));
  ASSERT_TRUE(st.Assign(j, w, 10).ok());
  const Job* job = st.FindJob(j);
  EXPECT_EQ(job->state, JobState::SCHEDULED);
  EXPECT_EQ(job->assigned_worker, w);
  EXPECT_EQ(job->generation, 1u);
  EXPECT_EQ(job->attempts, 1);
  EXPECT_EQ(job->first_scheduled, 10);
  EXPECT_EQ(st.FindWorker(w)->allocated, R(400, 300));
  EXPECT_TRUE(st.Pending().empty());
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Assign, RejectsIllegalPlacements) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId big = SubmitJob(st, Spec(2000, 100));
  JobId ok = SubmitJob(st, Spec(100, 100));
  EXPECT_FALSE(st.Assign(big, w, 0).ok());        // does not fit
  EXPECT_FALSE(st.Assign(ok, 99, 0).ok());        // no such worker
  EXPECT_FALSE(st.Assign(99, w, 0).ok());         // no such job
  ASSERT_TRUE(st.Assign(ok, w, 0).ok());
  EXPECT_FALSE(st.Assign(ok, w, 0).ok());         // not pending any more
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Assign, RefusesNonAliveWorker) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000), 0);
  JobId j = SubmitJob(st, Spec(100, 100));
  st.CheckWorkers(4 * kS, 3 * kS, 5 * kS);  // -> SUSPECT
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::SUSPECT);
  EXPECT_FALSE(st.Assign(j, w, 4 * kS).ok());
}

TEST(Lifecycle, FinishSuccessAndFailureReleaseResources) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(300, 300));
  JobId b = SubmitJob(st, Spec(300, 300));
  ASSERT_TRUE(st.Assign(a, w, 0).ok());
  ASSERT_TRUE(st.Assign(b, w, 0).ok());
  ASSERT_TRUE(st.ReportStarted(a, 1, w, 1).ok());
  EXPECT_EQ(st.FindJob(a)->state, JobState::RUNNING);
  ASSERT_TRUE(st.ReportFinished(a, 1, w, 0, 2).ok());
  ASSERT_TRUE(st.ReportFinished(b, 1, w, 3, 2).ok());  // nonzero exit
  EXPECT_EQ(st.FindJob(a)->state, JobState::SUCCEEDED);
  EXPECT_EQ(st.FindJob(b)->state, JobState::FAILED);
  EXPECT_EQ(st.FindJob(b)->exit_code, 3);
  EXPECT_EQ(st.FindWorker(w)->allocated, R(0, 0));
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Lifecycle, StaleGenerationReportIsIgnored) {
  ClusterState st;
  WorkerId w1 = AddWorker(st, "w1", R(1000, 1000));
  WorkerId w2 = AddWorker(st, "w2", R(1000, 1000));
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, w1, 0).ok());
  ASSERT_TRUE(st.MarkWorkerDead(w1, 1).ok());  // attempt 1 lost, requeued
  ASSERT_TRUE(st.Assign(j, w2, 2).ok());       // attempt 2, generation 2
  EXPECT_EQ(st.FindJob(j)->generation, 2u);
  // The zombie on w1 finishes attempt 1 late: must not complete the job.
  ASSERT_TRUE(st.ReportFinished(j, 1, w1, 0, 3).ok());
  EXPECT_EQ(st.FindJob(j)->state, JobState::SCHEDULED);
  EXPECT_EQ(st.counters().stale_reports_ignored, 1u);
  ASSERT_TRUE(st.ReportFinished(j, 2, w2, 0, 4).ok());
  EXPECT_EQ(st.FindJob(j)->state, JobState::SUCCEEDED);
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Cancel, PendingScheduledAndTerminal) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId p = SubmitJob(st, Spec(100, 100));
  JobId q = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(p, w, 0).ok());
  ASSERT_TRUE(st.Cancel(q, 1).ok());  // pending
  EXPECT_TRUE(st.Pending().empty());
  ASSERT_TRUE(st.Cancel(p, 1).ok());  // scheduled
  EXPECT_EQ(st.FindWorker(w)->allocated, R(0, 0));
  EXPECT_FALSE(st.Cancel(p, 2).ok());  // already terminal
  EXPECT_EQ(st.FindJob(p)->state, JobState::CANCELLED);
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(WorkerDeath, RequeuesAtFrontInIdOrderAndCountsAttempts) {
  ClusterState st;
  WorkerId w1 = AddWorker(st, "w1", R(1000, 1000));
  WorkerId w2 = AddWorker(st, "w2", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  JobId b = SubmitJob(st, Spec(100, 100));
  JobId c = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(a, w1, 0).ok());
  ASSERT_TRUE(st.Assign(b, w1, 0).ok());
  ASSERT_TRUE(st.Assign(c, w2, 0).ok());
  JobId d = SubmitJob(st, Spec(100, 100));  // already waiting in the queue
  ASSERT_TRUE(st.MarkWorkerDead(w1, 5).ok());
  EXPECT_EQ(st.FindWorker(w1)->status, WorkerStatus::DEAD);
  EXPECT_EQ(st.FindWorker(w1)->allocated, R(0, 0));
  EXPECT_EQ((std::vector<JobId>(st.Pending().begin(), st.Pending().end())),
            (std::vector<JobId>{a, b, d}));  // lost jobs jump the queue, in id order
  EXPECT_EQ(st.FindJob(a)->state, JobState::PENDING);
  EXPECT_EQ(st.FindJob(a)->attempts, 1);
  EXPECT_EQ(st.FindJob(c)->state, JobState::SCHEDULED);  // other worker untouched
  EXPECT_EQ(st.counters().jobs_lost, 2u);
  EXPECT_EQ(st.counters().jobs_rescheduled, 2u);
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(WorkerDeath, JobFailsWhenAttemptsExhausted) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId j = SubmitJob(st, Spec(100, 100, /*max_attempts=*/2));
  ASSERT_TRUE(st.Assign(j, w, 0).ok());
  ASSERT_TRUE(st.MarkWorkerDead(w, 1).ok());
  EXPECT_EQ(st.FindJob(j)->state, JobState::PENDING);
  // New incarnation of the worker takes it again, then dies again.
  auto reg = st.RegisterWorker("w", R(1000, 1000), {}, {}, 2);
  ASSERT_TRUE(reg.ok());
  ASSERT_TRUE(st.Assign(j, w, 2).ok());
  ASSERT_TRUE(st.MarkWorkerDead(w, 3).ok());
  EXPECT_EQ(st.FindJob(j)->state, JobState::FAILED);
  EXPECT_EQ(st.FindJob(j)->attempts, 2);
  EXPECT_TRUE(st.Pending().empty());
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(FailureDetector, SuspectThenDeadThresholds) {
  ClusterState st;
  WorkerId w = AddWorker(st, "w", R(1000, 1000), 0);
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, w, 0).ok());
  EXPECT_TRUE(st.CheckWorkers(2999 * kMillisecond, 3 * kS, 5 * kS).empty());
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::ALIVE);
  EXPECT_TRUE(st.CheckWorkers(3 * kS, 3 * kS, 5 * kS).empty());
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::SUSPECT);
  EXPECT_EQ(st.FindJob(j)->state, JobState::SCHEDULED);  // suspect keeps its jobs
  auto dead = st.CheckWorkers(5 * kS, 3 * kS, 5 * kS);
  EXPECT_EQ(dead, std::vector<WorkerId>{w});
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::DEAD);
  EXPECT_EQ(st.FindJob(j)->state, JobState::PENDING);
  EXPECT_TRUE(st.CheckWorkers(60 * kS, 3 * kS, 5 * kS).empty());  // dead only reported once
}

TEST(FailureDetector, SlowWorkerRecoversFromSuspect) {
  ClusterState st;
  auto reg = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0);
  WorkerId w = reg.value().worker;
  st.CheckWorkers(4 * kS, 3 * kS, 5 * kS);
  ASSERT_EQ(st.FindWorker(w)->status, WorkerStatus::SUSPECT);
  auto hb = st.Heartbeat(w, reg.value().epoch, {}, {}, 4 * kS);
  EXPECT_FALSE(hb.reregister);
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::ALIVE);
}

TEST(Fencing, StaleEpochHeartbeatIsRejected) {
  ClusterState st;
  auto r1 = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0);
  auto r2 = st.RegisterWorker("w", R(1000, 1000), {}, {}, 1);  // agent restarted
  ASSERT_TRUE(r1.ok() && r2.ok());
  EXPECT_EQ(r1.value().worker, r2.value().worker);  // identity is stable...
  EXPECT_GT(r2.value().epoch, r1.value().epoch);    // ...incarnation is not
  EXPECT_TRUE(st.Heartbeat(r1.value().worker, r1.value().epoch, {}, {}, 2).reregister);
  EXPECT_FALSE(st.Heartbeat(r2.value().worker, r2.value().epoch, {}, {}, 2).reregister);
  EXPECT_TRUE(st.Heartbeat(12345, 1, {}, {}, 2).reregister);  // unknown worker
}

TEST(Fencing, DeadWorkerCannotHeartbeatBackWithOldEpoch) {
  ClusterState st;
  auto reg = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0);
  WorkerId w = reg.value().worker;
  st.CheckWorkers(10 * kS, 3 * kS, 5 * kS);  // declared dead
  EXPECT_TRUE(st.Heartbeat(w, reg.value().epoch, {}, {}, 10 * kS).reregister);
}

// The partition scenario from DESIGN.md section 5: a "dead" worker is only partitioned and
// still running the job. After the job has moved on, the zombie must be fenced off.
TEST(Fencing, PartitionedWorkerReturnsAndZombieJobIsKilled) {
  ClusterState st;
  auto a = st.RegisterWorker("a", R(1000, 1000), {}, {}, 0).value();
  auto b = st.RegisterWorker("b", R(1000, 1000), {}, {}, 0).value();
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, a.worker, 0).ok());
  RunningReport on_a{j, 1};
  ASSERT_FALSE(st.Heartbeat(a.worker, a.epoch, std::span(&on_a, 1), {}, 1 * kS).reregister);
  // a goes silent; b keeps heartbeating.
  st.Heartbeat(b.worker, b.epoch, {}, {}, 4 * kS);
  auto dead = st.CheckWorkers(6 * kS, 3 * kS, 5 * kS);
  ASSERT_EQ(dead, std::vector<WorkerId>{a.worker});
  ASSERT_TRUE(st.Assign(j, b.worker, 6 * kS).ok());  // attempt 2 on b, generation 2
  // Partition heals. a heartbeats with its old epoch: rejected.
  EXPECT_TRUE(st.Heartbeat(a.worker, a.epoch, std::span(&on_a, 1), {}, 7 * kS).reregister);
  // a re-registers still reporting the zombie attempt: told to kill it.
  auto re = st.RegisterWorker("a", R(1000, 1000), std::span(&on_a, 1), {}, 7 * kS);
  ASSERT_TRUE(re.ok());
  EXPECT_EQ(re.value().kill, std::vector<JobId>{j});
  EXPECT_GT(re.value().epoch, a.epoch);
  EXPECT_EQ(st.FindJob(j)->assigned_worker, b.worker);  // job still belongs to b
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Heartbeat, DeliversAssignmentsUntilWorkerReportsThem) {
  ClusterState st;
  auto w = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, w.worker, 0).ok());
  auto h1 = st.Heartbeat(w.worker, w.epoch, {}, {}, 1);
  ASSERT_EQ(h1.assignments.size(), 1u);
  EXPECT_EQ(h1.assignments[0].job, j);
  EXPECT_EQ(h1.assignments[0].generation, 1u);
  auto h2 = st.Heartbeat(w.worker, w.epoch, {}, {}, 2);  // response was "lost": resent
  EXPECT_EQ(h2.assignments.size(), 1u);
  RunningReport rr{j, 1};
  auto h3 = st.Heartbeat(w.worker, w.epoch, std::span(&rr, 1), {}, 3);
  EXPECT_TRUE(h3.assignments.empty());
  EXPECT_EQ(st.FindJob(j)->state, JobState::RUNNING);
}

TEST(Heartbeat, CompletionIsAckedAndIdempotent) {
  ClusterState st;
  auto w = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, w.worker, 0).ok());
  CompletedReport c{j, 1, 0};
  auto h1 = st.Heartbeat(w.worker, w.epoch, {}, std::span(&c, 1), 1);
  EXPECT_EQ(h1.acked, std::vector<JobId>{j});
  EXPECT_EQ(st.FindJob(j)->state, JobState::SUCCEEDED);
  auto h2 = st.Heartbeat(w.worker, w.epoch, {}, std::span(&c, 1), 2);  // ack lost, resent
  EXPECT_EQ(h2.acked, std::vector<JobId>{j});
  EXPECT_EQ(st.FindJob(j)->state, JobState::SUCCEEDED);
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Heartbeat, UnknownRunningJobsAreKilled) {
  ClusterState st;
  auto w = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Cancel(j, 0).ok());
  RunningReport cancelled{j, 1}, ghost{999, 1};
  RunningReport both[] = {cancelled, ghost};
  auto h = st.Heartbeat(w.worker, w.epoch, both, {}, 1);
  EXPECT_EQ(h.kill, (std::vector<JobId>{j, 999}));
}

TEST(Heartbeat, RunningJobThatVanishesIsLost) {
  ClusterState st;
  auto w = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
  JobId j = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(j, w.worker, 0).ok());
  RunningReport rr{j, 1};
  st.Heartbeat(w.worker, w.epoch, std::span(&rr, 1), {}, 1);
  ASSERT_EQ(st.FindJob(j)->state, JobState::RUNNING);
  st.Heartbeat(w.worker, w.epoch, {}, {}, 2);  // neither running nor completed
  EXPECT_EQ(st.FindJob(j)->state, JobState::PENDING);
  EXPECT_EQ(st.counters().jobs_lost, 1u);
}

TEST(Register, ReconcileAdoptsKillsAndLoses) {
  ClusterState st;
  auto w = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
  JobId kept = SubmitJob(st, Spec(100, 100));
  JobId gone = SubmitJob(st, Spec(100, 100));
  JobId done = SubmitJob(st, Spec(100, 100));
  for (JobId id : {kept, gone, done}) ASSERT_TRUE(st.Assign(id, w.worker, 0).ok());
  RunningReport run[] = {{kept, 1}, {777, 1}};  // kept is real; 777 is unknown to us
  CompletedReport comp[] = {{done, 1, 0}};
  auto re = st.RegisterWorker("w", R(1000, 1000), run, comp, 1);
  ASSERT_TRUE(re.ok());
  EXPECT_EQ(re.value().kill, std::vector<JobId>{777});
  EXPECT_EQ(re.value().acked, std::vector<JobId>{done});
  EXPECT_EQ(st.FindJob(kept)->state, JobState::RUNNING);    // adopted
  EXPECT_EQ(st.FindJob(done)->state, JobState::SUCCEEDED);  // result delivered, not re-run
  EXPECT_EQ(st.FindJob(gone)->state, JobState::PENDING);    // not reported: lost, requeued
  EXPECT_EQ(st.FindWorker(w.worker)->allocated, R(100, 100));
  EXPECT_EQ(st.CheckInvariants(), "");
}

TEST(Register, UnknownWorkerGetsAllReportedJobsKilled) {
  ClusterState st;
  RunningReport run{5, 1};
  auto re = st.RegisterWorker("fresh", R(1000, 1000), std::span(&run, 1), {}, 0);
  ASSERT_TRUE(re.ok());
  EXPECT_EQ(re.value().kill, std::vector<JobId>{5});
}

TEST(Register, RejectsBadInput) {
  ClusterState st;
  EXPECT_FALSE(st.RegisterWorker("", R(1, 1), {}, {}, 0).ok());
  EXPECT_FALSE(st.RegisterWorker("w", R(0, 1), {}, {}, 0).ok());
}

// ---- journal / replay ----------------------------------------------------------------------

TEST(Journal, EveryTransitionIsAppendedBeforeItTakesEffect) {
  RecordingJournal j;
  ClusterState st(&j);
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(a, w, 0).ok());
  ASSERT_TRUE(st.ReportFinished(a, 1, w, 0, 1).ok());
  std::vector<RecordType> types;
  for (auto& r : j.log) types.push_back(r.type);
  EXPECT_EQ(types, (std::vector<RecordType>{RecordType::REGISTER, RecordType::SUBMIT,
                                            RecordType::ASSIGN, RecordType::FINISH}));
}

TEST(Journal, FailedAppendLeavesStateUntouched) {
  RecordingJournal j;
  ClusterState st(&j);
  j.fail = true;
  EXPECT_FALSE(st.Submit(Spec(100, 100), 0).ok());
  EXPECT_EQ(st.JobCount(), 0u);
  j.fail = false;
  JobId a = SubmitJob(st, Spec(100, 100));
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  j.fail = true;
  EXPECT_FALSE(st.Assign(a, w, 0).ok());
  EXPECT_EQ(st.FindJob(a)->state, JobState::PENDING);
  EXPECT_EQ(st.FindWorker(w)->allocated, R(0, 0));
}

TEST(Journal, WorkerDeathIsOneBatch) {
  RecordingJournal j;
  ClusterState st(&j);
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  for (int i = 0; i < 3; ++i) {
    JobId id = SubmitJob(st, Spec(100, 100));
    ASSERT_TRUE(st.Assign(id, w, 0).ok());
  }
  int before = j.appends;
  ASSERT_TRUE(st.MarkWorkerDead(w, 1).ok());
  EXPECT_EQ(j.appends, before + 1);  // three LOST records, one fsync
}

// Compare everything replay is supposed to reproduce.
void ExpectSameJobs(const ClusterState& a, const ClusterState& b) {
  auto ja = a.ListJobs(), jb = b.ListJobs();
  ASSERT_EQ(ja.size(), jb.size());
  for (size_t i = 0; i < ja.size(); ++i) {
    EXPECT_EQ(ja[i].id, jb[i].id);
    EXPECT_EQ(ja[i].state, jb[i].state) << "job " << ja[i].id;
    EXPECT_EQ(ja[i].attempts, jb[i].attempts) << "job " << ja[i].id;
    EXPECT_EQ(ja[i].generation, jb[i].generation) << "job " << ja[i].id;
    EXPECT_EQ(ja[i].assigned_worker, jb[i].assigned_worker) << "job " << ja[i].id;
    EXPECT_EQ(ja[i].submitted, jb[i].submitted);
    EXPECT_EQ(ja[i].first_scheduled, jb[i].first_scheduled);
  }
  EXPECT_EQ(std::vector<JobId>(a.Pending().begin(), a.Pending().end()),
            std::vector<JobId>(b.Pending().begin(), b.Pending().end()));
}

TEST(Replay, ReproducesStateAndMarksWorkersUnknown) {
  RecordingJournal j;
  ClusterState st(&j);
  WorkerId w1 = AddWorker(st, "w1", R(1000, 1000));
  WorkerId w2 = AddWorker(st, "w2", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  JobId b = SubmitJob(st, Spec(100, 100));
  JobId c = SubmitJob(st, Spec(100, 100));
  SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(a, w1, 1).ok());
  ASSERT_TRUE(st.Assign(b, w2, 1).ok());
  ASSERT_TRUE(st.Assign(c, w2, 1).ok());
  ASSERT_TRUE(st.ReportFinished(a, 1, w1, 0, 2).ok());
  ASSERT_TRUE(st.MarkWorkerDead(w2, 3).ok());

  ClusterState re;
  for (const Record& r : j.log) re.Replay(r);
  re.FinishReplay(100);
  ExpectSameJobs(st, re);
  EXPECT_EQ(re.CheckInvariants(), "");
  EXPECT_EQ(re.counters().replay_skipped, 0u);
  for (const Worker& w : re.Workers()) {
    EXPECT_EQ(w.status, WorkerStatus::SUSPECT);  // unknown until it re-registers
    EXPECT_EQ(w.last_heartbeat, 100);
  }
  // Ids and epochs continue past the replayed ones, never colliding with them.
  JobId n = SubmitJob(re, Spec(100, 100));
  EXPECT_GT(n, 4u);
  auto reg = re.RegisterWorker("w1", R(1000, 1000), {}, {}, 101).value();
  EXPECT_EQ(reg.worker, w1);
  EXPECT_GT(reg.epoch, st.FindWorker(w1)->epoch);
}

TEST(Replay, GhostWorkerThatNeverReturnsIsDeclaredDead) {
  RecordingJournal j;
  ClusterState st(&j);
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(a, w, 1).ok());

  ClusterState re;
  for (const Record& r : j.log) re.Replay(r);
  re.FinishReplay(1000 * kS);
  EXPECT_EQ(re.FindJob(a)->state, JobState::SCHEDULED);
  EXPECT_TRUE(re.CheckWorkers(1002 * kS, 3 * kS, 5 * kS).empty());
  EXPECT_EQ(re.CheckWorkers(1005 * kS, 3 * kS, 5 * kS), std::vector<WorkerId>{w});
  EXPECT_EQ(re.FindJob(a)->state, JobState::PENDING);
}

TEST(Replay, WorkerThatReturnsAfterRestartKeepsItsRunningJob) {
  RecordingJournal j;
  ClusterState st(&j);
  WorkerId w = AddWorker(st, "w", R(1000, 1000));
  JobId a = SubmitJob(st, Spec(100, 100));
  ASSERT_TRUE(st.Assign(a, w, 1).ok());

  ClusterState re;
  for (const Record& r : j.log) re.Replay(r);
  re.FinishReplay(100);
  RunningReport rr{a, 1};
  auto reg = re.RegisterWorker("w", R(1000, 1000), std::span(&rr, 1), {}, 101);
  ASSERT_TRUE(reg.ok());
  EXPECT_TRUE(reg.value().kill.empty());
  EXPECT_EQ(re.FindJob(a)->state, JobState::RUNNING);
  EXPECT_EQ(re.FindJob(a)->assigned_worker, w);
  EXPECT_EQ(re.counters().jobs_lost, 0u);
}

// ---- randomised model check ----------------------------------------------------------------

// Random legal-ish operations; invariants must hold after every step, and replaying the journal
// must reproduce the same job table.
TEST(RandomOps, InvariantsHoldAndReplayMatches) {
  for (uint64_t seed = 1; seed <= 25; ++seed) {
    sim::Rng rng(seed);
    RecordingJournal j;
    ClusterState st(&j);
    TimeUs now = 0;
    std::vector<std::pair<std::string, Epoch>> workers;
    for (int step = 0; step < 400; ++step) {
      now += static_cast<TimeUs>(rng.Uniform(0, 2 * kS));
      switch (rng.Uniform(0, 9)) {
        case 0: {  // (re-)register one of 4 workers, reporting nothing
          std::string name = "w" + std::to_string(rng.Uniform(0, 3));
          auto r = st.RegisterWorker(name, R(1000 * (1 + rng.Uniform(0, 3)), 4096), {}, {}, now);
          ASSERT_TRUE(r.ok());
          break;
        }
        case 1: case 2: case 3:
          SubmitJob(st, Spec(static_cast<int64_t>(rng.Uniform(1, 4)) * 100,
                             static_cast<int64_t>(rng.Uniform(1, 8)) * 256,
                             static_cast<int>(rng.Uniform(1, 3))),
                    now);
          break;
        case 4: case 5: {  // try to place the queue head on a random worker
          auto head = st.PendingHead();
          auto ws = st.Workers();
          if (head && !ws.empty()) st.Assign(*head, ws[rng.Uniform(0, ws.size() - 1)].id, now);
          break;
        }
        case 6: {  // finish a random active job
          for (const Job& job : st.ListJobs())
            if (job.state == JobState::SCHEDULED || job.state == JobState::RUNNING) {
              st.ReportFinished(job.id, job.generation, job.assigned_worker,
                                static_cast<int32_t>(rng.Uniform(0, 1)), now);
              break;
            }
          break;
        }
        case 7: {  // cancel a random non-terminal job
          for (const Job& job : st.ListJobs())
            if (!IsTerminal(job.state) && rng.Uniform(0, 3) == 0) {
              st.Cancel(job.id, now);
              break;
            }
          break;
        }
        case 8:
          st.CheckWorkers(now, 3 * kS, 5 * kS);
          break;
        case 9: {  // stale report from a bogus generation must be harmless
          auto jobs = st.ListJobs();
          if (!jobs.empty())
            st.ReportFinished(jobs[0].id, 999, jobs[0].assigned_worker, 0, now);
          break;
        }
      }
      ASSERT_EQ(st.CheckInvariants(), "") << "seed " << seed << " step " << step;
    }
    ClusterState re;
    for (const Record& r : j.log) re.Replay(r);
    ASSERT_EQ(re.counters().replay_skipped, 0u) << "seed " << seed;
    ASSERT_EQ(re.CheckInvariants(), "") << "seed " << seed;
    ExpectSameJobs(st, re);
  }
}

}  // namespace
