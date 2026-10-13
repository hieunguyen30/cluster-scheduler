// ControlPlane-level tests that need no gRPC: failure-detector self-monitoring.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "cluster_harness.h"  // WaitFor
#include "control/control_plane.h"
#include "test_util.h"

using namespace kestrel;
using namespace kestrel::testing_util;
using std::chrono::milliseconds;

namespace {

// A journal whose appends can be made slow, like an fsync that stalls on a busy disk.
class SlowJournal : public Journal {
 public:
  Status Append(std::span<const Record>) override {
    int d = delay_ms.load();
    if (d > 0) std::this_thread::sleep_for(milliseconds(d));
    return Status::Ok();
  }
  std::atomic<int> delay_ms{0};
};

ControlPlaneConfig FastConfig() {
  ControlPlaneConfig c;
  c.suspect_after = 500 * kMillisecond;
  c.dead_after = 1000 * kMillisecond;
  c.detector_interval = 50 * kMillisecond;
  c.scheduler_tick = 50 * kMillisecond;
  return c;
}

// Regression test for a bug found by the restart tests: the journal fsync runs under the global
// mutex, so a slow fsync freezes heartbeat handling. The detector then woke up, saw every
// worker "silent" for longer than dead_after, and declared them all dead before the heartbeats
// queued behind the lock could be processed.
TEST(DetectorSelfMonitoring, ControlPlaneStallDoesNotKillHealthyWorkers) {
  SlowJournal journal;
  ControlPlane cp(FastConfig(), &journal);
  cp.Start();
  auto reg = cp.Register("w", {1000, 1000}, {}, {});
  ASSERT_TRUE(reg.ok());
  const WorkerId w = reg.value().worker;
  const Epoch e = reg.value().epoch;
  ASSERT_FALSE(cp.Heartbeat(w, e, {}, {}).reregister);

  // Freeze the control plane for 2 s (2x dead_after) by making one journal append slow while
  // it holds the lock. No heartbeat can be processed meanwhile: the worker is NOT at fault.
  journal.delay_ms = 2000;
  std::thread stall([&] { cp.SubmitJob(Spec(100, 100)); });
  std::this_thread::sleep_for(milliseconds(2300));  // detector ticks right after the stall
  journal.delay_ms = 0;
  stall.join();

  // The first heartbeat after the stall must still be accepted: the worker was never declared dead.
  EXPECT_FALSE(cp.Heartbeat(w, e, {}, {}).reregister);
  auto workers = cp.ListWorkers();
  ASSERT_EQ(workers.size(), 1u);
  EXPECT_NE(workers[0].status, WorkerStatus::DEAD);
  EXPECT_GE(cp.Snapshot().detector_stalls, 1u);
  cp.Stop();
}

// The credit must not blunt real failure detection: a worker that really goes silent is still
// declared dead on time afterwards.
TEST(DetectorSelfMonitoring, RealSilenceIsStillDetected) {
  SlowJournal journal;
  ControlPlane cp(FastConfig(), &journal);
  cp.Start();
  auto reg = cp.Register("w", {1000, 1000}, {}, {}).value();
  journal.delay_ms = 600;
  cp.SubmitJob(Spec(100, 100));  // a 600 ms stall that triggers a credit
  journal.delay_ms = 0;
  // Now the worker truly stops heartbeating.
  auto t0 = std::chrono::steady_clock::now();
  ASSERT_TRUE(WaitFor([&] { return cp.ListWorkers()[0].status == WorkerStatus::DEAD; },
                      std::chrono::seconds(10)));
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LE(s, 3.0);
  (void)reg;
  cp.Stop();
}

TEST(CreditSilence, ShiftsHeartbeatsForwardButNeverPastNow) {
  ClusterState st;
  WorkerId a = AddWorker(st, "a", R(1000, 1000), 0);
  WorkerId b = AddWorker(st, "b", R(1000, 1000), 0);
  st.CheckWorkers(10 * kSecond, 3 * kSecond, 100 * kSecond);  // both SUSPECT, last_hb still 0
  ASSERT_TRUE(st.Heartbeat(b, st.FindWorker(b)->epoch, {}, {}, 9 * kSecond).reregister == false);
  st.CreditSilence(4 * kSecond, 10 * kSecond);
  EXPECT_EQ(st.FindWorker(a)->last_heartbeat, 4 * kSecond);
  EXPECT_EQ(st.FindWorker(b)->last_heartbeat, 10 * kSecond);  // 9 s + 4 s, capped at now
  st.CreditSilence(-5, 10 * kSecond);                           // negative / zero is a no-op
  EXPECT_EQ(st.FindWorker(a)->last_heartbeat, 4 * kSecond);
}

TEST(CreditSilence, DeadWorkersAreNotResurrected) {
  ClusterState st;
  WorkerId a = AddWorker(st, "a", R(1000, 1000), 0);
  st.CheckWorkers(10 * kSecond, 3 * kSecond, 5 * kSecond);
  ASSERT_EQ(st.FindWorker(a)->status, WorkerStatus::DEAD);
  st.CreditSilence(100 * kSecond, 200 * kSecond);
  EXPECT_EQ(st.FindWorker(a)->status, WorkerStatus::DEAD);
  EXPECT_EQ(st.FindWorker(a)->last_heartbeat, 0);
}

}  // namespace
