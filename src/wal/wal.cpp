#include "wal/wal.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#include "common/log.h"

namespace kestrel {

namespace {

constexpr char kMagic[4] = {'K', 'W', 'A', 'L'};
constexpr uint32_t kVersion = 1;
constexpr size_t kFileHeaderSize = 8;
constexpr size_t kFrameHeaderSize = 12;  // len | crc32c(len) | crc32c(payload)
constexpr uint32_t kMaxFrameBytes = 64u << 20;

// ---- CRC32C (Castagnoli), table driven ---------------------------------------------------
const std::array<uint32_t, 256>& CrcTable() {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  return table;
}

// ---- little-endian primitives ----------------------------------------------------------------
void PutU8(std::string& s, uint8_t v) { s.push_back(static_cast<char>(v)); }
void PutU32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutU64(std::string& s, uint64_t v) {
  for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
void PutStr(std::string& s, const std::string& v) {
  PutU32(s, static_cast<uint32_t>(v.size()));
  s.append(v);
}

bool GetU8(const char*& p, const char* end, uint8_t* v) {
  if (end - p < 1) return false;
  *v = static_cast<uint8_t>(*p++);
  return true;
}
bool GetU32(const char*& p, const char* end, uint32_t* v) {
  if (end - p < 4) return false;
  uint32_t x = 0;
  for (int i = 0; i < 4; ++i) x |= static_cast<uint32_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  p += 4;
  *v = x;
  return true;
}
bool GetU64(const char*& p, const char* end, uint64_t* v) {
  if (end - p < 8) return false;
  uint64_t x = 0;
  for (int i = 0; i < 8; ++i) x |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  p += 8;
  *v = x;
  return true;
}
bool GetStr(const char*& p, const char* end, std::string* v) {
  uint32_t n;
  if (!GetU32(p, end, &n) || static_cast<uint64_t>(end - p) < n) return false;
  v->assign(p, n);
  p += n;
  return true;
}

bool WriteAll(int fd, const char* data, size_t len) {
  while (len > 0) {
    ssize_t n = ::write(fd, data, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool ReadFile(int fd, size_t size, std::string* out) {
  out->resize(size);
  size_t got = 0;
  while (got < size) {
    ssize_t n = ::pread(fd, out->data() + got, size - got, static_cast<off_t>(got));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
  out->resize(got);
  return true;
}

void SyncParentDir(const std::string& path) {
  std::string dir = ".";
  auto slash = path.find_last_of('/');
  if (slash != std::string::npos) dir = slash == 0 ? "/" : path.substr(0, slash);
  int dfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
}

double SecondsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

}  // namespace

uint32_t Crc32c(const void* data, size_t len) {
  const auto& t = CrcTable();
  const auto* p = static_cast<const uint8_t*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

std::string EncodeRecord(const Record& r) {
  std::string s;
  PutU8(s, static_cast<uint8_t>(r.type));
  PutU64(s, static_cast<uint64_t>(r.time));
  PutU64(s, r.job);
  PutU64(s, r.worker);
  PutU64(s, r.generation);
  PutU64(s, r.epoch);
  PutU32(s, static_cast<uint32_t>(r.exit_code));
  PutStr(s, r.spec.command);
  PutU32(s, static_cast<uint32_t>(r.spec.args.size()));
  for (const auto& a : r.spec.args) PutStr(s, a);
  PutU64(s, static_cast<uint64_t>(r.spec.request.cpu_millis));
  PutU64(s, static_cast<uint64_t>(r.spec.request.mem_mib));
  PutU32(s, static_cast<uint32_t>(r.spec.max_attempts));
  PutStr(s, r.spec.request_id);
  PutStr(s, r.name);
  PutU64(s, static_cast<uint64_t>(r.capacity.cpu_millis));
  PutU64(s, static_cast<uint64_t>(r.capacity.mem_mib));
  return s;
}

bool DecodeRecord(const char*& p, const char* end, Record* out) {
  Record r;
  uint8_t type;
  uint64_t u;
  uint32_t n, w;
  if (!GetU8(p, end, &type) || type < 1 || type > 6) return false;
  r.type = static_cast<RecordType>(type);
  if (!GetU64(p, end, &u)) return false;
  r.time = static_cast<TimeUs>(u);
  if (!GetU64(p, end, &r.job) || !GetU64(p, end, &r.worker) || !GetU64(p, end, &r.generation) ||
      !GetU64(p, end, &r.epoch) || !GetU32(p, end, &w))
    return false;
  r.exit_code = static_cast<int32_t>(w);
  if (!GetStr(p, end, &r.spec.command) || !GetU32(p, end, &n)) return false;
  if (n > static_cast<uint64_t>(end - p)) return false;  // each arg needs >= 4 bytes
  for (uint32_t i = 0; i < n; ++i) {
    std::string a;
    if (!GetStr(p, end, &a)) return false;
    r.spec.args.push_back(std::move(a));
  }
  if (!GetU64(p, end, &u)) return false;
  r.spec.request.cpu_millis = static_cast<int64_t>(u);
  if (!GetU64(p, end, &u)) return false;
  r.spec.request.mem_mib = static_cast<int64_t>(u);
  if (!GetU32(p, end, &w)) return false;
  r.spec.max_attempts = static_cast<int>(static_cast<int32_t>(w));
  if (!GetStr(p, end, &r.spec.request_id) || !GetStr(p, end, &r.name)) return false;
  if (!GetU64(p, end, &u)) return false;
  r.capacity.cpu_millis = static_cast<int64_t>(u);
  if (!GetU64(p, end, &u)) return false;
  r.capacity.mem_mib = static_cast<int64_t>(u);
  *out = std::move(r);
  return true;
}

Wal::Wal(std::string path, int fd, WalOptions opts)
    : path_(std::move(path)), fd_(fd), opts_(opts) {
  if (opts_.sync == WalSync::INTERVAL) sync_thread_ = std::thread([this] { SyncLoop(); });
}

Wal::~Wal() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_ = true;
  }
  sync_cv_.notify_all();
  if (sync_thread_.joinable()) sync_thread_.join();
  if (fd_ >= 0) {
    if (opts_.sync != WalSync::NONE) ::fdatasync(fd_);
    ::close(fd_);
  }
}

void Wal::SyncLoop() {
  std::unique_lock<std::mutex> lk(mu_);
  while (!stop_) {
    sync_cv_.wait_for(lk, std::chrono::milliseconds(opts_.interval_ms), [this] { return stop_; });
    if (dirty_ && !poisoned_.load()) {
      dirty_ = false;
      lk.unlock();  // do not block Append during the (slow) fdatasync
      if (::fdatasync(fd_) != 0) {
        poisoned_.store(true);
        log::Write(log::Level::ERROR, "wal", "background fdatasync failed; log poisoned",
                   {{"errno", std::strerror(errno)}});
      }
      lk.lock();
    }
  }
}

Result<std::unique_ptr<Wal>> Wal::Open(const std::string& path, WalOptions opts,
                                       std::vector<Record>* recovered, WalRecoveryInfo* info) {
  WalRecoveryInfo local;
  if (info == nullptr) info = &local;
  *info = WalRecoveryInfo{};
  recovered->clear();

  int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd < 0) return Status::Error("open " + path + ": " + std::strerror(errno));
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return Status::Error("fstat: " + std::string(std::strerror(errno)));
  }
  const uint64_t size = static_cast<uint64_t>(st.st_size);
  info->file_bytes = size;

  auto fail = [&](const std::string& msg) -> Result<std::unique_ptr<Wal>> {
    ::close(fd);
    return Status::Error(msg);
  };

  if (size == 0) {
    std::string hdr(kMagic, 4);
    PutU32(hdr, kVersion);
    if (!WriteAll(fd, hdr.data(), hdr.size()) || ::fsync(fd) != 0)
      return fail("cannot initialise WAL header: " + std::string(std::strerror(errno)));
    SyncParentDir(path);  // make the file's existence durable too
    info->file_bytes = hdr.size();
    return std::unique_ptr<Wal>(new Wal(path, fd, opts));
  }

  std::string data;
  if (!ReadFile(fd, size, &data)) return fail("read WAL: " + std::string(std::strerror(errno)));
  if (data.size() < kFileHeaderSize) {
    // A crash while creating the file: nothing was ever acknowledged. Re-initialise.
    if (::ftruncate(fd, 0) != 0) return fail("ftruncate: " + std::string(std::strerror(errno)));
    ::close(fd);
    return Open(path, opts, recovered, info);
  }
  if (std::memcmp(data.data(), kMagic, 4) != 0) return fail("not a Kestrel WAL (bad magic)");
  const char* hp = data.data() + 4;
  uint32_t version = 0;
  GetU32(hp, data.data() + data.size(), &version);
  if (version != kVersion) return fail("unsupported WAL version " + std::to_string(version));

  uint64_t off = kFileHeaderSize, good_end = kFileHeaderSize;
  bool torn = false;
  while (off < data.size()) {
    const uint64_t remaining = data.size() - off;
    if (remaining < kFrameHeaderSize) {
      torn = true;  // partial frame header at EOF
      break;
    }
    const char* p = data.data() + off;
    const char* end = data.data() + data.size();
    uint32_t len, hdr_crc, payload_crc;
    GetU32(p, end, &len);
    GetU32(p, end, &hdr_crc);
    GetU32(p, end, &payload_crc);
    uint8_t len_bytes[4];
    std::memcpy(len_bytes, data.data() + off, 4);
    if (Crc32c(len_bytes, 4) != hdr_crc) {
      // Either a zero-filled tail (size extended, data never written) or real corruption.
      bool all_zero = std::all_of(data.begin() + static_cast<long>(off), data.end(),
                                  [](char c) { return c == 0; });
      if (all_zero) {
        torn = true;
        break;
      }
      return fail("WAL corrupt at offset " + std::to_string(off) + " (frame header checksum)");
    }
    if (len > kMaxFrameBytes)
      return fail("WAL corrupt at offset " + std::to_string(off) + " (absurd frame length)");
    if (remaining < kFrameHeaderSize + len) {
      torn = true;  // header intact, payload cut short by the crash
      break;
    }
    if (Crc32c(p, len) != payload_crc) {
      if (off + kFrameHeaderSize + len == data.size()) {
        torn = true;  // last frame is garbage: a partially persisted write
        break;
      }
      return fail("WAL corrupt at offset " + std::to_string(off) +
                  " (payload checksum) with valid data after it; refusing to drop it");
    }
    const char* q = p;
    const char* qend = p + len;
    uint32_t count = 0;
    if (!GetU32(q, qend, &count)) return fail("WAL corrupt: bad frame payload");
    std::vector<Record> batch;
    for (uint32_t i = 0; i < count; ++i) {
      Record r;
      if (!DecodeRecord(q, qend, &r))
        return fail("WAL corrupt at offset " + std::to_string(off) + " (undecodable record)");
      batch.push_back(std::move(r));
    }
    if (q != qend) return fail("WAL corrupt: trailing bytes in frame");
    for (Record& r : batch) recovered->push_back(std::move(r));
    ++info->frames;
    off += kFrameHeaderSize + len;
    good_end = off;
  }
  info->records = recovered->size();
  if (torn) {
    info->torn_bytes_truncated = data.size() - good_end;
    log::Write(log::Level::WARN, "wal", "truncating torn tail",
               {{"bytes", log::S(info->torn_bytes_truncated)}, {"at", log::S(good_end)}});
    if (::ftruncate(fd, static_cast<off_t>(good_end)) != 0 || ::fsync(fd) != 0)
      return fail("cannot truncate torn tail: " + std::string(std::strerror(errno)));
  }
  if (::lseek(fd, 0, SEEK_END) < 0) return fail("lseek: " + std::string(std::strerror(errno)));
  return std::unique_ptr<Wal>(new Wal(path, fd, opts));
}

Status Wal::Append(std::span<const Record> records) {
  if (poisoned_.load()) return Status::Error("WAL poisoned by an earlier I/O error");
  std::string payload;
  PutU32(payload, static_cast<uint32_t>(records.size()));
  for (const Record& r : records) payload += EncodeRecord(r);
  if (payload.size() > kMaxFrameBytes) return Status::Error("WAL batch too large");

  std::string frame;
  PutU32(frame, static_cast<uint32_t>(payload.size()));
  PutU32(frame, Crc32c(frame.data(), 4));
  PutU32(frame, Crc32c(payload.data(), payload.size()));
  frame += payload;

  const auto t0 = std::chrono::steady_clock::now();
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (poisoned_.load()) return Status::Error("WAL poisoned by an earlier I/O error");
    if (inject_failure_.exchange(false) || !WriteAll(fd_, frame.data(), frame.size())) {
      poisoned_.store(true);
      return Status::Error("WAL write failed");
    }
    if (opts_.sync == WalSync::EVERY) {
      if (::fdatasync(fd_) != 0) {
        poisoned_.store(true);
        return Status::Error("WAL fdatasync failed");
      }
    } else {
      dirty_ = true;
    }
  }
  bytes_ += frame.size();
  ++frames_;
  latency_.Observe(SecondsSince(t0));
  return Status::Ok();
}

}  // namespace kestrel
