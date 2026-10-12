// Tiny flag parser: `--key value`, `--key=value`, and bare `--switch`. Positional arguments
// (and everything after a literal `--`) are collected in order.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace kestrel {

class Flags {
 public:
  Flags(int argc, char** argv, std::vector<std::string> switches = {}) {
    for (int i = 1; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--") {
        for (++i; i < argc; ++i) positional_.push_back(argv[i]);
        break;
      }
      if (a.rfind("--", 0) != 0) {
        positional_.push_back(a);
        continue;
      }
      a = a.substr(2);
      auto eq = a.find('=');
      if (eq != std::string::npos) {
        values_[a.substr(0, eq)] = a.substr(eq + 1);
      } else if (std::find(switches.begin(), switches.end(), a) != switches.end() ||
                 i + 1 >= argc) {
        values_[a] = "true";
      } else {
        values_[a] = argv[++i];
      }
    }
  }
  bool Has(const std::string& k) const { return values_.count(k) > 0; }
  std::string Str(const std::string& k, const std::string& def = "") const {
    auto it = values_.find(k);
    return it == values_.end() ? def : it->second;
  }
  long long Int(const std::string& k, long long def) const {
    auto it = values_.find(k);
    return it == values_.end() ? def : std::strtoll(it->second.c_str(), nullptr, 10);
  }
  double Double(const std::string& k, double def) const {
    auto it = values_.find(k);
    return it == values_.end() ? def : std::strtod(it->second.c_str(), nullptr);
  }
  const std::vector<std::string>& positional() const { return positional_; }

 private:
  std::map<std::string, std::string> values_;
  std::vector<std::string> positional_;
};

}  // namespace kestrel
