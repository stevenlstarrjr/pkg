#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "pkg/result.hpp"

namespace pkg {

struct LayoutConfig {
  std::string ports_dir = "ports";
  std::string groups_dir = "groups";
  std::string store_dir = "store";
  std::string build_dir = "build";
  std::string profile_dir = "profile";
  std::string current_profile = "profile/current";
  std::string lockfile = "ports.lock";
};

struct ResolverConfig {
  std::string strategy = "strict";
};

struct BuildConfig {
  int jobs = 0;
  bool keep_build_dirs = false;
  bool keep_failed_build_dirs = true;
};

struct ProfileConfig {
  std::string activate_symlink = "/usr/local";
  std::string activate_target = "/usr/ports/profile/current";
  int generations_to_keep = 5;
};

struct PackageConfig {
  std::string source_dir = "build/packages";
};

struct Config {
  LayoutConfig layout;
  ResolverConfig resolver;
  BuildConfig build;
  ProfileConfig profile;
  PackageConfig packages;
};

class ConfigStore {
 public:
  static constexpr const char* kConfigFilename = "pkg.toml";

  static Result<Config> load(const std::filesystem::path& root);
  static Status validate(const std::filesystem::path& root);
};

}  // namespace pkg
