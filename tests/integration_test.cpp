#include <gtest/gtest.h>
#include <signal.h>

#include <fstream>
#include <set>

#include "cluster_harness.h"
#include "test_util.h"

using namespace kestrel;
using namespace kestrel::testing_util;

namespace {

TEST(Integration, SubmittedJobRunsToSuccess) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Submit("true", {});
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED));
  Job job = h.Get(j);
  EXPECT_EQ(job.attempts, 1);
  EXPECT_EQ(job.exit_code, 0);
  // `started` may legitimately be unset: RUNNING is only learned from a heartbeat, and this job
  // can finish within one heartbeat interval (SCHEDULED -> SUCCEEDED directly).
  EXPECT_NE(job.finished, kUnsetTime);
  EXPECT_EQ(h.CheckInvariants(), "");
}

TEST(Integration, NonZeroExitFailsWithoutRetry) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Sh("exit 7");
  ASSERT_TRUE(h.WaitState(j, JobState::FAILED));
  EXPECT_EQ(h.Get(j).exit_code, 7);
  EXPECT_EQ(h.Get(j).attempts, 1);  // application failures are not retried
}

TEST(Integration, MissingExecutableFailsWith127) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Submit("/definitely/not/a/binary", {});
  ASSERT_TRUE(h.WaitState(j, JobState::FAILED));
  EXPECT_EQ(h.Get(j).exit_code, 127);
}

TEST(Integration, SignalKilledJobReportsShellConvention) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Sh("kill -9 $$");
  ASSERT_TRUE(h.WaitState(j, JobState::FAILED));
  EXPECT_EQ(h.Get(j).exit_code, 128 + 9);
}

TEST(Integration, ManyJobsSpreadAcrossWorkersAndAllFinish) {
  Harness h;
  h.StartControl();
  for (int i = 0; i < 3; ++i) h.AddWorker("w" + std::to_string(i), {2000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(3));
  std::vector<JobId> ids;
  for (int i = 0; i < 40; ++i) ids.push_back(h.Sh("sleep 0.05", 500, 256));
  for (JobId id : ids) ASSERT_TRUE(h.WaitState(id, JobState::SUCCEEDED)) << "job " << id;
  std::set<WorkerId> used;
  for (JobId id : ids) used.insert(h.Get(id).assigned_worker);
  EXPECT_GE(used.size(), 2u);
  EXPECT_EQ(h.CheckInvariants(), "");
  for (const Worker& w : h.cp->ListWorkers()) EXPECT_EQ(w.allocated, (Resources{0, 0}));
}

TEST(Integration, NeverOverCommitsAWorker) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1", {1000, 4096});
  ASSERT_TRUE(h.WaitForWorkers(1));
  std::vector<JobId> ids;
  for (int i = 0; i < 6; ++i) ids.push_back(h.Sh("sleep 0.2", 600, 64));  // only one fits at a time
  bool over = false;
  size_t max_running = 0;
  WaitFor([&] {
    size_t running = 0, done = 0;
    for (JobId id : ids) {
      JobState s = h.Get(id).state;
      running += (s == JobState::RUNNING || s == JobState::SCHEDULED);
      done += s == JobState::SUCCEEDED;
    }
    max_running = std::max(max_running, running);
    for (const Worker& w : h.cp->ListWorkers())
      if (!w.allocated.FitsIn(w.capacity)) over = true;
    return done == ids.size();
  });
  EXPECT_FALSE(over);
  EXPECT_EQ(max_running, 1u);
}

TEST(Integration, UnschedulableJobWaitsUntilABigEnoughWorkerAppears) {
  Harness h;
  h.StartControl();
  h.AddWorker("small", {1000, 1024});
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId big = h.Sh("true", 4000, 512);
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_EQ(h.Get(big).state, JobState::PENDING);
  h.AddWorker("big", {8000, 16384});
  ASSERT_TRUE(h.WaitState(big, JobState::SUCCEEDED));
  EXPECT_EQ(h.cp->ListWorkers().size(), 2u);
}

TEST(Integration, CancelRunningJobKillsTheProcess) {
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  std::string pidfile = ::testing::TempDir() + "kestrel_cancel_pid";
  std::remove(pidfile.c_str());
  JobId j = h.Sh("echo $$ > " + pidfile + "; sleep 60");
  ASSERT_TRUE(h.WaitState(j, JobState::RUNNING));
  long pid = 0;
  ASSERT_TRUE(WaitFor([&] {
    std::ifstream f(pidfile);
    return static_cast<bool>(f >> pid) && pid > 0;
  }));
  ASSERT_EQ(::kill(static_cast<pid_t>(pid), 0), 0);  // alive
  v1::CancelJobRequest req;
  req.set_job_id(j);
  grpc::ClientContext ctx;
  v1::CancelJobResponse resp;
  ASSERT_TRUE(h.stub->CancelJob(&ctx, req, &resp).ok());
  EXPECT_EQ(h.Get(j).state, JobState::CANCELLED);
  // The cancel order reaches the worker on a heartbeat; the process then dies.
  EXPECT_TRUE(WaitFor([&] { return ::kill(static_cast<pid_t>(pid), 0) != 0; },
                      std::chrono::seconds(10)));
  EXPECT_TRUE(WaitFor([&] { return h.workers[0]->RunningCount() == 0; }));
  for (const Worker& w : h.cp->ListWorkers()) EXPECT_EQ(w.allocated, (Resources{0, 0}));
}

TEST(Integration, CancelPendingJobNeverRuns) {
  Harness h;
  h.StartControl();  // no workers: job stays pending
  JobId j = h.Sh("true");
  ASSERT_EQ(h.Get(j).state, JobState::PENDING);
  v1::CancelJobRequest req;
  req.set_job_id(j);
  grpc::ClientContext ctx;
  v1::CancelJobResponse resp;
  ASSERT_TRUE(h.stub->CancelJob(&ctx, req, &resp).ok());
  EXPECT_EQ(h.Get(j).state, JobState::CANCELLED);
  grpc::ClientContext ctx2;
  EXPECT_EQ(h.stub->CancelJob(&ctx2, req, &resp).error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  v1::CancelJobRequest missing;
  missing.set_job_id(9999);
  grpc::ClientContext ctx3;
  EXPECT_EQ(h.stub->CancelJob(&ctx3, missing, &resp).error_code(), grpc::StatusCode::NOT_FOUND);
}

TEST(Integration, SubmitWithSameRequestIdIsIdempotent) {
  Harness h;
  h.StartControl();
  JobId a = h.Submit("true", {}, 100, 64, 0, "key-1");
  JobId b = h.Submit("true", {}, 100, 64, 0, "key-1");
  JobId c = h.Submit("true", {}, 100, 64, 0, "key-2");
  EXPECT_EQ(a, b);
  EXPECT_NE(a, c);
  EXPECT_EQ(h.cp->ListJobs(std::nullopt, 0).size(), 2u);
}

TEST(Integration, InvalidSubmitIsRejected) {
  Harness h;
  h.StartControl();
  v1::SubmitJobRequest req;
  req.mutable_spec()->set_command("true");  // zero CPU/memory
  grpc::ClientContext ctx;
  v1::SubmitJobResponse resp;
  EXPECT_EQ(h.stub->SubmitJob(&ctx, req, &resp).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(Integration, ListJobsAndWorkersRpcs) {
  Harness h;
  h.StartControl();
  h.AddWorker("alpha");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Sh("true");
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED));
  v1::ListJobsRequest lreq;
  lreq.set_state(v1::JOB_STATE_SUCCEEDED);
  v1::ListJobsResponse lresp;
  grpc::ClientContext c1;
  ASSERT_TRUE(h.stub->ListJobs(&c1, lreq, &lresp).ok());
  ASSERT_EQ(lresp.jobs_size(), 1);
  EXPECT_EQ(lresp.jobs(0).id(), j);
  v1::ListWorkersResponse wresp;
  grpc::ClientContext c2;
  ASSERT_TRUE(h.stub->ListWorkers(&c2, v1::ListWorkersRequest(), &wresp).ok());
  ASSERT_EQ(wresp.workers_size(), 1);
  EXPECT_EQ(wresp.workers(0).name(), "alpha");
  EXPECT_EQ(wresp.workers(0).status(), v1::WORKER_STATUS_ALIVE);
}

TEST(Integration, RunningJobSurvivesWorkerHeartbeatingNormally) {
  // A job longer than the dead timeout must not be declared lost while heartbeats flow.
  Harness h;
  h.StartControl();
  h.AddWorker("w1");
  ASSERT_TRUE(h.WaitForWorkers(1));
  JobId j = h.Sh("sleep " + ScaledSeconds(2.5));  // > 2x dead_after
  ASSERT_TRUE(h.WaitState(j, JobState::SUCCEEDED, std::chrono::seconds(20)));
  EXPECT_EQ(h.Get(j).attempts, 1);
  EXPECT_EQ(h.cp->Snapshot().counters.jobs_lost, 0u);
}

}  // namespace
