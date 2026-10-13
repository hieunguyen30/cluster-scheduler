#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <thread>

#include "core/cluster_state.h"
#include "sim/rng.h"
#include "test_util.h"
#include "wal/wal.h"

using namespace kestrel;
using namespace kestrel::testing_util;

namespace {

std::string TempPath(const std::string& tag) {
  static std::atomic<int> n{0};
  std::string p = ::testing::TempDir() + "kestrel_wal_" + tag + "_" + std::to_string(::getpid()) +
                  "_" + std::to_string(n++);
  std::remove(p.c_str());
  return p;
}

std::string Slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}
void Spit(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
}

Record Submit(JobId id, const std::string& cmd = "sleep", int nargs = 1) {
  Record r;
  r.type = RecordType::SUBMIT;
  r.time = 1000 + static_cast<TimeUs>(id);
  r.job = id;
  r.spec.command = cmd;
  for (int i = 0; i < nargs; ++i) r.spec.args.push_back("arg" + std::to_string(i));
  r.spec.request = {250, 128};
  r.spec.max_attempts = 3;
  r.spec.request_id = "rid-" + std::to_string(id);
  return r;
}

std::vector<Record> OpenAll(const std::string& path, WalRecoveryInfo* info = nullptr,
                            Status* status = nullptr, WalSync sync = WalSync::NONE) {
  std::vector<Record> recs;
  WalOptions o;
  o.sync = sync;
  auto w = Wal::Open(path, o, &recs, info);
  if (status != nullptr) *status = w.status();
  return recs;
}

TEST(Crc32c, KnownVector) {
  EXPECT_EQ(Crc32c("123456789", 9), 0xE3069283u);
  EXPECT_EQ(Crc32c("", 0), 0u);
}

TEST(Codec, RoundTripsEveryField) {
  Record r;
  r.type = RecordType::REGISTER;
  r.time = -5;  // negative times must survive
  r.job = 1ull << 63;
  r.worker = 7;
  r.generation = 9;
  r.epoch = 11;
  r.exit_code = -3;
  r.spec.command = "/bin/echo";
  r.spec.args = {"", "with space", "new\nline", std::string("nul\0byte", 8)};
  r.spec.request = {123456789, 987654321};
  r.spec.max_attempts = 42;
  r.spec.request_id = "k";
  r.name = "pod-x";
  r.capacity = {8000, 32768};
  std::string enc = EncodeRecord(r);
  const char* p = enc.data();
  Record out;
  ASSERT_TRUE(DecodeRecord(p, enc.data() + enc.size(), &out));
  EXPECT_EQ(p, enc.data() + enc.size());
  EXPECT_TRUE(r == out);
}

TEST(Codec, RejectsTruncatedAndGarbageInput) {
  std::string enc = EncodeRecord(Submit(1));
  for (size_t cut = 0; cut < enc.size(); ++cut) {
    const char* p = enc.data();
    Record out;
    EXPECT_FALSE(DecodeRecord(p, enc.data() + cut, &out)) << "cut " << cut;
  }
  sim::Rng rng(5);
  for (int i = 0; i < 2000; ++i) {  // random bytes must never crash or hang the decoder
    std::string junk;
    for (int k = 0; k < 64; ++k) junk.push_back(static_cast<char>(rng.Uniform(0, 255)));
    const char* p = junk.data();
    Record out;
    DecodeRecord(p, junk.data() + junk.size(), &out);
  }
}

TEST(Wal, FreshFileThenReopenReturnsEverythingAppended) {
  std::string path = TempPath("basic");
  std::vector<Record> none;
  {
    auto w = Wal::Open(path, {}, &none);
    ASSERT_TRUE(w.ok()) << w.status().message();
    EXPECT_TRUE(none.empty());
    std::vector<Record> b1 = {Submit(1), Submit(2)};
    std::vector<Record> b2 = {Submit(3, "cmd", 40)};
    ASSERT_TRUE(w.value()->Append(b1).ok());
    ASSERT_TRUE(w.value()->Append(b2).ok());
    EXPECT_EQ(w.value()->frames_written(), 2u);
    EXPECT_EQ(w.value()->AppendLatency().count, 2u);
  }
  WalRecoveryInfo info;
  auto recs = OpenAll(path, &info);
  ASSERT_EQ(recs.size(), 3u);
  EXPECT_TRUE(recs[0] == Submit(1));
  EXPECT_TRUE(recs[2] == Submit(3, "cmd", 40));
  EXPECT_EQ(info.frames, 2u);
  EXPECT_EQ(info.torn_bytes_truncated, 0u);
}

TEST(Wal, AppendsAfterReopenExtendTheLog) {
  std::string path = TempPath("reopen");
  for (int round = 0; round < 3; ++round) {
    std::vector<Record> got;
    auto w = Wal::Open(path, {}, &got);
    ASSERT_TRUE(w.ok());
    EXPECT_EQ(got.size(), static_cast<size_t>(round));
    std::vector<Record> b = {Submit(static_cast<JobId>(round + 1))};
    ASSERT_TRUE(w.value()->Append(b).ok());
  }
  EXPECT_EQ(OpenAll(path).size(), 3u);
}

// The core crash-safety property. Cut the file at EVERY byte offset: recovery must succeed,
// return exactly the batches that were fully written, and leave a log that can be appended to.
TEST(Wal, TruncationAtEveryByteRecoversExactlyTheWholeBatches) {
  std::string path = TempPath("sweep");
  std::vector<std::vector<Record>> batches = {
      {Submit(1)}, {Submit(2), Submit(3), Submit(4)}, {Submit(5, "x", 0)}, {Submit(6, "y", 9)}};
  std::vector<size_t> boundaries = {8};  // file size after each complete batch
  {
    std::vector<Record> none;
    auto w = Wal::Open(path, {}, &none);
    ASSERT_TRUE(w.ok());
    for (auto& b : batches) {
      ASSERT_TRUE(w.value()->Append(b).ok());
      boundaries.push_back(Slurp(path).size());
    }
  }
  const std::string full = Slurp(path);
  ASSERT_EQ(boundaries.back(), full.size());
  for (size_t cut = 0; cut <= full.size(); ++cut) {
    Spit(path, full.substr(0, cut));
    size_t whole = 0;  // number of complete batches within `cut` bytes
    size_t expect_records = 0;
    for (size_t i = 0; i < batches.size(); ++i)
      if (boundaries[i + 1] <= cut) {
        ++whole;
        expect_records += batches[i].size();
      }
    WalRecoveryInfo info;
    Status st = Status::Ok();
    auto recs = OpenAll(path, &info, &st);
    ASSERT_TRUE(st.ok()) << "cut " << cut << ": " << st.message();
    ASSERT_EQ(recs.size(), expect_records) << "cut " << cut;
    // Atomicity: never part of a batch.
    size_t idx = 0;
    for (size_t i = 0; i < whole; ++i)
      for (const Record& r : batches[i]) ASSERT_TRUE(recs[idx++] == r) << "cut " << cut;
    // The recovered log is clean: a new append lands right after the last good batch.
    {
      std::vector<Record> again;
      auto w = Wal::Open(path, {}, &again);
      ASSERT_TRUE(w.ok());
      ASSERT_EQ(again.size(), expect_records);
      std::vector<Record> extra = {Submit(99)};
      ASSERT_TRUE(w.value()->Append(extra).ok());
    }
    auto after = OpenAll(path);
    ASSERT_EQ(after.size(), expect_records + 1) << "cut " << cut;
    ASSERT_TRUE(after.back() == Submit(99));
  }
}

TEST(Wal, ZeroFilledTailIsATornWriteNotCorruption) {
  // After a crash some filesystems extend the file size but never write the data blocks.
  std::string path = TempPath("zeros");
  {
    std::vector<Record> none;
    auto w = Wal::Open(path, {}, &none);
    std::vector<Record> b = {Submit(1)};
    ASSERT_TRUE(w.value()->Append(b).ok());
  }
  std::string data = Slurp(path);
  Spit(path, data + std::string(4096, '\0'));
  WalRecoveryInfo info;
  Status st = Status::Ok();
  auto recs = OpenAll(path, &info, &st);
  ASSERT_TRUE(st.ok()) << st.message();
  EXPECT_EQ(recs.size(), 1u);
  EXPECT_EQ(info.torn_bytes_truncated, 4096u);
  EXPECT_EQ(Slurp(path).size(), data.size());
}

TEST(Wal, CorruptionInTheMiddleRefusesToStart) {
  std::string path = TempPath("corrupt");
  size_t second_frame_start = 0;
  {
    std::vector<Record> none;
    auto w = Wal::Open(path, {}, &none);
    for (JobId i = 1; i <= 4; ++i) {
      std::vector<Record> b = {Submit(i)};
      ASSERT_TRUE(w.value()->Append(b).ok());
      if (i == 1) second_frame_start = Slurp(path).size();
    }
  }
  std::string data = Slurp(path);
  data[second_frame_start + 12 + 5] ^= 0x40;  // flip a payload bit in frame 2 of 4
  Spit(path, data);
  Status st = Status::Ok();
  auto recs = OpenAll(path, nullptr, &st);
  EXPECT_FALSE(st.ok());
  EXPECT_NE(st.message().find("corrupt"), std::string::npos) << st.message();
  EXPECT_EQ(Slurp(path).size(), data.size());  // refused: nothing was truncated away
}

TEST(Wal, DamagedLastFrameIsTreatedAsTornTail) {
  std::string path = TempPath("lastbad");
  {
    std::vector<Record> none;
    auto w = Wal::Open(path, {}, &none);
    for (JobId i = 1; i <= 3; ++i) {
      std::vector<Record> b = {Submit(i)};
      ASSERT_TRUE(w.value()->Append(b).ok());
    }
  }
  std::string data = Slurp(path);
  data[data.size() - 3] ^= 0x01;
  Spit(path, data);
  Status st = Status::Ok();
  auto recs = OpenAll(path, nullptr, &st);
  ASSERT_TRUE(st.ok());
  EXPECT_EQ(recs.size(), 2u);
}

TEST(Wal, RejectsForeignFilesAndFutureVersions) {
  std::string path = TempPath("foreign");
  Spit(path, "this is definitely not a write ahead log, it is a poem");
  Status st = Status::Ok();
  OpenAll(path, nullptr, &st);
  EXPECT_FALSE(st.ok());
  std::string path2 = TempPath("version");
  Spit(path2, std::string("KWAL") + std::string("\x09\x00\x00\x00", 4));
  OpenAll(path2, nullptr, &st);
  EXPECT_FALSE(st.ok());
}

TEST(Wal, IoErrorPoisonsTheLogAndRefusesFurtherAppends) {
  std::string path = TempPath("poison");
  std::vector<Record> none;
  auto w = Wal::Open(path, {}, &none);
  ASSERT_TRUE(w.ok());
  std::vector<Record> b = {Submit(1)};
  ASSERT_TRUE(w.value()->Append(b).ok());
  w.value()->InjectWriteFailureForTest();
  EXPECT_FALSE(w.value()->Append(b).ok());
  EXPECT_TRUE(w.value()->poisoned());
  EXPECT_FALSE(w.value()->Append(b).ok());  // never "retry and hope": fsync errors can lose data
  w.value().reset();
  EXPECT_EQ(OpenAll(path).size(), 1u);  // only the acknowledged record exists
}

TEST(Wal, AllSyncModesPersistAcrossReopen) {
  for (WalSync mode : {WalSync::EVERY, WalSync::INTERVAL, WalSync::NONE}) {
    std::string path = TempPath("modes");
    {
      std::vector<Record> none;
      WalOptions o;
      o.sync = mode;
      o.interval_ms = 5;
      auto w = Wal::Open(path, o, &none);
      ASSERT_TRUE(w.ok());
      for (JobId i = 1; i <= 20; ++i) {
        std::vector<Record> b = {Submit(i)};
        ASSERT_TRUE(w.value()->Append(b).ok());
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));  // let INTERVAL sync run
    }
    EXPECT_EQ(OpenAll(path).size(), 20u);
  }
}

TEST(Wal, ConcurrentAppendsAreAllRecoveredIntact) {
  std::string path = TempPath("threads");
  std::vector<Record> none;
  auto w = Wal::Open(path, {WalSync::NONE, 10}, &none);
  ASSERT_TRUE(w.ok());
  std::vector<std::thread> ts;
  for (int t = 0; t < 4; ++t)
    ts.emplace_back([&, t] {
      for (int i = 0; i < 200; ++i) {
        std::vector<Record> b = {Submit(static_cast<JobId>(t * 1000 + i + 1))};
        ASSERT_TRUE(w.value()->Append(b).ok());
      }
    });
  for (auto& t : ts) t.join();
  w.value().reset();
  auto recs = OpenAll(path);
  ASSERT_EQ(recs.size(), 800u);
  std::set<JobId> ids;
  for (const Record& r : recs) {
    ids.insert(r.job);
    ASSERT_TRUE(r == Submit(r.job));
  }
  EXPECT_EQ(ids.size(), 800u);
}

// ---- WAL + ClusterState --------------------------------------------------------------------

// "What happens if the control plane crashes between writing the WAL and replying to the
// client?" Model it: a journal that makes the record durable and then reports failure, as if the
// process died before applying it or answering.
class CrashAfterAppend : public Journal {
 public:
  explicit CrashAfterAppend(Journal* inner) : inner_(inner) {}
  Status Append(std::span<const Record> r) override {
    Status s = inner_->Append(r);
    if (s.ok() && armed) {
      armed = false;
      return Status::Error("simulated crash after durable append");
    }
    return s;
  }
  bool armed = false;

 private:
  Journal* inner_;
};

TEST(Recovery, CrashBetweenWalWriteAndReplyDoesNotDuplicateTheJobOnRetry) {
  std::string path = TempPath("crash_reply");
  JobSpec spec = Spec(100, 100);
  spec.request_id = "client-op-1";
  {
    std::vector<Record> none;
    auto wal = Wal::Open(path, {}, &none);
    ASSERT_TRUE(wal.ok());
    CrashAfterAppend crashy(wal.value().get());
    ClusterState st(&crashy);
    crashy.armed = true;
    EXPECT_FALSE(st.Submit(spec, 1).ok());  // client sees an error / timeout
    EXPECT_EQ(st.JobCount(), 0u);           // the dying process never applied it
  }
  // Restart: the record IS in the log, so the job exists although nobody was ever told so.
  std::vector<Record> recs;
  auto wal = Wal::Open(path, {}, &recs);
  ASSERT_TRUE(wal.ok());
  ClusterState st(wal.value().get());
  for (const Record& r : recs) st.Replay(r);
  st.FinishReplay(100);
  ASSERT_EQ(st.JobCount(), 1u);
  // The client retries with the same idempotency key and gets the same job: no duplicate.
  auto again = st.Submit(spec, 101);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value(), 1u);
  EXPECT_EQ(st.JobCount(), 1u);
  // Without an idempotency key the retry would create a second job: at-least-once submission.
  JobSpec nokey = Spec(100, 100);
  EXPECT_EQ(SubmitJob(st, nokey), 2u);
}

TEST(Recovery, FullLifecycleThroughRealWalReplaysToTheSameState) {
  std::string path = TempPath("lifecycle");
  std::vector<Job> before;
  std::vector<JobId> pending_before;
  {
    std::vector<Record> none;
    auto wal = Wal::Open(path, {}, &none);
    ClusterState st(wal.value().get());
    WorkerId w1 = AddWorker(st, "w1", R(1000, 1000));
    WorkerId w2 = AddWorker(st, "w2", R(1000, 1000));
    for (int i = 0; i < 8; ++i) SubmitJob(st, Spec(200, 100));
    for (JobId id = 1; id <= 6; ++id) ASSERT_TRUE(st.Assign(id, id % 2 ? w1 : w2, 10).ok());
    ASSERT_TRUE(st.ReportFinished(1, 1, w1, 0, 20).ok());
    ASSERT_TRUE(st.ReportFinished(2, 1, w2, 5, 20).ok());
    ASSERT_TRUE(st.Cancel(7, 21).ok());
    ASSERT_TRUE(st.MarkWorkerDead(w2, 30).ok());
    before = st.ListJobs();
    pending_before.assign(st.Pending().begin(), st.Pending().end());
  }
  std::vector<Record> recs;
  auto wal = Wal::Open(path, {}, &recs);
  ASSERT_TRUE(wal.ok());
  ClusterState st;
  for (const Record& r : recs) st.Replay(r);
  EXPECT_EQ(st.counters().replay_skipped, 0u);
  EXPECT_EQ(st.CheckInvariants(), "");
  auto after = st.ListJobs();
  ASSERT_EQ(before.size(), after.size());
  for (size_t i = 0; i < before.size(); ++i) {
    EXPECT_EQ(before[i].state, after[i].state) << "job " << before[i].id;
    EXPECT_EQ(before[i].attempts, after[i].attempts);
    EXPECT_EQ(before[i].generation, after[i].generation);
    EXPECT_EQ(before[i].exit_code, after[i].exit_code);
  }
  EXPECT_EQ(pending_before, std::vector<JobId>(st.Pending().begin(), st.Pending().end()));
}

TEST(Recovery, HeartbeatWithAPreCrashEpochIsRejectedUntilReregistration) {
  std::string path = TempPath("epochs");
  Epoch old_epoch;
  WorkerId w;
  {
    std::vector<Record> none;
    auto wal = Wal::Open(path, {}, &none);
    ClusterState st(wal.value().get());
    auto reg = st.RegisterWorker("w", R(1000, 1000), {}, {}, 0).value();
    w = reg.worker;
    old_epoch = reg.epoch;
  }
  std::vector<Record> recs;
  auto wal = Wal::Open(path, {}, &recs);
  ClusterState st(wal.value().get());
  for (const Record& r : recs) st.Replay(r);
  st.FinishReplay(100);
  EXPECT_TRUE(st.Heartbeat(w, old_epoch, {}, {}, 101).reregister);
  auto reg = st.RegisterWorker("w", R(1000, 1000), {}, {}, 102).value();
  EXPECT_EQ(reg.worker, w);
  EXPECT_GT(reg.epoch, old_epoch);
  EXPECT_FALSE(st.Heartbeat(w, reg.epoch, {}, {}, 103).reregister);
}

}  // namespace
