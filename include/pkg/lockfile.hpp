#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "pkg/config.hpp"
#include "pkg/result.hpp"

namespace pkg {

struct LockEntry {
  std::string name;
  std::string version;
  std::string status;
  std::string recipe;
  std::vector<std::string> deps;
  std::string store;
  std::string log;
  std::string error;
  int build_seconds = 0;
};

struct Lockfile {
  int schema = 1;
  std::string state;
  std::string run_command;
  long long started_unix = 0;
  long long finished_unix = 0;
  int summary_planned = 0;
  int summary_built = 0;
  int summary_reused = 0;
  int summary_failed = 0;
  int summary_skipped = 0;
  std::vector<LockEntry> entries;
};

class LockfileStore {
 public:
  static Result<Lockfile> load(const std::filesystem::path& root,
                               const Config& config);
  static Status save(const std::filesystem::path& root,
                     const Config& config,
                     const Lockfile& lockfile);
};

}  // namespace pkg
