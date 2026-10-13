// Write-ahead log (DESIGN.md section 6).
//
// File layout:   "KWAL" u32 version=1  then frames:
//   frame := u32 payload_len | u32 crc32c(payload) | payload          (little endian)
//   payload := u32 record_count | record*                              (one Journal::Append call)
// One frame per Append means a batch (e.g. all LOST records for a dead worker) is atomic: after a
// crash either every record of the batch is recovered or none is.
//
// Recovery rules:
//   * A bad frame that reaches or passes the end of the file is a TORN TAIL (the crash happened
//     mid-write): it is truncated and recovery succeeds. Nothing in it was ever acknowledged,
//     because Append returns only after the write (and, in EVERY mode, the fsync) completed.
//   * A bad frame with more data after it is CORRUPTION, not a torn write. Recovery refuses to
//     start rather than silently dropping acknowledged records that follow.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "common/histogram.h"
#include "core/model.h"

namespace kestrel {

enum class WalSync {
  EVERY,     // fdatasync before every Append returns: acknowledged == durable (default)
  INTERVAL,  // fdatasync every `interval_ms` in the background: a crash can lose that window
  NONE,      // never fsync (benchmarks only; survives a process crash but not an OS crash)
};

struct WalOptions {
  WalSync sync = WalSync::EVERY;
  int interval_ms = 10;
};

struct WalRecoveryInfo {
  uint64_t frames = 0;
  uint64_t records = 0;
  uint64_t torn_bytes_truncated = 0;
  uint64_t file_bytes = 0;
};

// Binary record codec (exposed for tests).
std::string EncodeRecord(const Record& r);
bool DecodeRecord(const char*& p, const char* end, Record* out);
uint32_t Crc32c(const void* data, size_t len);

class Wal : public Journal {
 public:
  // Opens (creating if needed) `path`, replays it into `recovered` and truncates a torn tail.
  static Result<std::unique_ptr<Wal>> Open(const std::string& path, WalOptions opts,
                                           std::vector<Record>* recovered,
                                           WalRecoveryInfo* info = nullptr);
  ~Wal() override;

  // Appends one frame. Returns ok only once the frame is written (and fsynced in EVERY mode).
  // After any I/O error the log is poisoned and every later Append fails: after a failed
  // fsync the kernel may have dropped the dirty pages, so retrying would lie about durability.
  Status Append(std::span<const Record> records) override;

  Histogram::Snapshot AppendLatency() const { return latency_.Take(); }
  uint64_t bytes_written() const { return bytes_.load(); }
  uint64_t frames_written() const { return frames_.load(); }
  bool poisoned() const { return poisoned_.load(); }
  const std::string& path() const { return path_; }

  // Test hook: make the next write fail as if the disk errored.
  void InjectWriteFailureForTest() { inject_failure_.store(true); }

 private:
  Wal(std::string path, int fd, WalOptions opts);
  void SyncLoop();

  std::string path_;
  int fd_;
  WalOptions opts_;
  std::mutex mu_;
  std::atomic<bool> poisoned_{false};
  std::atomic<bool> inject_failure_{false};
  std::atomic<uint64_t> bytes_{0}, frames_{0};
  Histogram latency_{Histogram::LatencyBounds()};

  // INTERVAL mode
  std::thread sync_thread_;
  std::condition_variable sync_cv_;
  bool stop_ = false;
  bool dirty_ = false;
};

}  // namespace kestrel
