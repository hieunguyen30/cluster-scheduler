// Minimal structured logger: one line per event, `ts level component msg key=value ...`.
// Job and worker ids are passed as key=value fields so logs can be grepped by id.
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace kestrel::log {

enum class Level { DEBUG = 0, INFO, WARN, ERROR };

void SetLevel(Level l);
Level GetLevel();

using Field = std::pair<std::string, std::string>;

// Thread-safe. Values containing spaces are quoted.
void Write(Level l, const char* component, const std::string& msg,
           std::initializer_list<Field> fields = {});

template <typename T>
std::string S(T v) {
  return std::to_string(v);
}

}  // namespace kestrel::log
