// Control-plane restart as seen from the network: a vanished pod leaves black-holed TCP
// connections (no RST). Found on kind: workers took ~4.6 s to notice, longer than dead_after.
#include <gtest/gtest.h>

#include <atomic>

#include "cluster_harness.h"
#include "tcp_proxy.h"

using namespace kestrel;
using namespace kestrel::testing_util;
using Clock = std::chrono::steady_clock;

namespace {

TEST(Reconnect, WorkerRecoversPromptlyFromABlackholedConnection) {
  Harness h;
  h.StartControl();
  TcpProxy proxy(h.port);  // workers dial the proxy, never the control plane directly
  h.AddWorker("w1", {4000, 8192}, "127.0.0.1:" + std::to_string(proxy.port()));
  h.AddWorker("w2", {4000, 8192}, "127.0.0.1:" + std::to_string(proxy.port()));
  ASSERT_TRUE(h.WaitForWorkers(2));

  // The control plane's pod disappears: its connections stay open but nothing flows.
  proxy.SetBlackhole(true);
  h.CrashControl();
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  h.StartControl("127.0.0.1:" + std::to_string(h.port));  // new pod, same Service address
  proxy.SetBlackhole(false);                                // network reachable again...
  auto t0 = Clock::now();                                   // ...but old connections stay dead
  ASSERT_TRUE(h.WaitForWorkers(2));                         // both re-registered with the new cp
  double s = std::chrono::duration<double>(Clock::now() - t0).count();
  std::printf("[measured] black-holed connection -> workers re-registered: %.2f s "
              "(rebuild after 3 failed RPCs, 0.5 s RPC deadline, 0.1 s heartbeat)\n", s);
  // Without channel rebuild or keepalive this takes as long as TCP's own timeouts (minutes).
  EXPECT_LE(s, 4.5 * TimeScale());
}

TEST(Reconnect, WorkerStartedBeforeTheControlPlaneConnectsWhenItAppears) {
  Harness h;
  // Reserve a port by starting then stopping the control plane.
  h.StartControl();
  const int port = h.port;
  h.CrashControl();
  h.AddWorker("early", {4000, 8192}, "127.0.0.1:" + std::to_string(port));
  std::this_thread::sleep_for(std::chrono::milliseconds(700));  // several failed attempts
  h.StartControl("127.0.0.1:" + std::to_string(port));
  EXPECT_TRUE(h.WaitForWorkers(1));
}

// Deterministic check of the recovery grace, at the state level (no detector thread: a fake
// clock that jumps looks like a control-plane pause to the stall self-monitor, which is the
// intended behaviour).
TEST(RecoveryGrace, ReplayedWorkersGetExtraTimeBeforeTheyCanBeDeclaredDead) {
  std::vector<Record> log;
  {
    struct Rec : Journal {
      std::vector<Record>* out;
      Status Append(std::span<const Record> r) override {
        out->insert(out->end(), r.begin(), r.end());
        return Status::Ok();
      }
    } rec;
    rec.out = &log;
    ClusterState st(&rec);
    ASSERT_TRUE(st.RegisterWorker("ghost", {1000, 1000}, {}, {}, 0).ok());
  }
  const TimeUs restart = 1000 * kSecond, grace = 10 * kSecond;
  ClusterState st;
  for (const Record& r : log) st.Replay(r);
  st.FinishReplay(restart + grace);  // exactly what ControlPlane::Recover does
  const WorkerId w = st.FindWorkerByName("ghost")->id;
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::SUSPECT);
  EXPECT_EQ(st.FindWorker(w)->epoch, 0u);
  // 14 s after the restart: past dead_after (5 s) but still inside grace + dead_after (15 s).
  EXPECT_TRUE(st.CheckWorkers(restart + 14 * kSecond, 3 * kSecond, 5 * kSecond).empty());
  EXPECT_EQ(st.FindWorker(w)->status, WorkerStatus::SUSPECT);
  EXPECT_EQ(st.CheckWorkers(restart + 15 * kSecond, 3 * kSecond, 5 * kSecond),
            std::vector<WorkerId>{w});
}

// ControlPlane::Recover must pass the grace through (clock injected).
TEST(RecoveryGrace, RecoverAppliesTheConfiguredGrace) {
  const TimeUs t = 777 * kSecond;
  ControlPlaneConfig cfg;
  cfg.recovery_grace = 10 * kSecond;
  cfg.clock = [t] { return t; };
  std::vector<Record> log;
  Record reg;
  reg.type = RecordType::REGISTER;
  reg.worker = 1;
  reg.epoch = 1;
  reg.name = "ghost";
  reg.capacity = {1000, 1000};
  log.push_back(reg);
  ControlPlane cp(cfg);
  cp.Recover(log);
  auto ws = cp.ListWorkers();
  ASSERT_EQ(ws.size(), 1u);
  EXPECT_EQ(ws[0].last_heartbeat, t + 10 * kSecond);
}

TEST(RecoveryGrace, ReRegisteringDuringGraceMakesTheWorkerAliveImmediately) {
  std::atomic<TimeUs> now{500 * kSecond};
  ControlPlaneConfig cfg;
  cfg.recovery_grace = 10 * kSecond;
  cfg.clock = [&] { return now.load(); };
  ControlPlane first(cfg);
  ASSERT_TRUE(first.Register("w", {1000, 1000}, {}, {}).ok());
  std::vector<Record> none;
  ControlPlane cp(cfg);
  cp.Recover(none);  // (no journal here; the point is the second registration below)
  auto r = cp.Register("w", {1000, 1000}, {}, {});
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(cp.ListWorkers()[0].status, WorkerStatus::ALIVE);
}

}  // namespace
