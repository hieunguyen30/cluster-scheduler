#include "common/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace kestrel::log {

namespace {
std::atomic<Level> g_level{Level::INFO};
std::mutex g_mu;
const char* Name(Level l) {
  switch (l) {
    case Level::DEBUG: return "DEBUG";
    case Level::INFO: return "INFO";
    case Level::WARN: return "WARN";
    case Level::ERROR: return "ERROR";
  }
  return "?";
}
}  // namespace

void SetLevel(Level l) { g_level.store(l); }
Level GetLevel() { return g_level.load(); }

void Write(Level l, const char* component, const std::string& msg,
           std::initializer_list<Field> fields) {
  if (l < g_level.load()) return;
  using namespace std::chrono;
  auto us = duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
  std::string line = std::to_string(us / 1000000) + "." + [&] {
    char b[8];
    std::snprintf(b, sizeof b, "%06lld", static_cast<long long>(us % 1000000));
    return std::string(b);
  }();
  line += " ";
  line += Name(l);
  line += " ";
  line += component;
  line += " ";
  line += msg;
  for (const Field& f : fields) {
    line += " " + f.first + "=";
    if (f.second.find(' ') != std::string::npos)
      line += "\"" + f.second + "\"";
    else
      line += f.second;
  }
  line += "\n";
  std::lock_guard<std::mutex> lk(g_mu);
  std::fputs(line.c_str(), stderr);
}

}  // namespace kestrel::log
