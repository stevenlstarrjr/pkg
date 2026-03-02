#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "pkg/config.hpp"
#include "pkg/result.hpp"

namespace pkg {

struct VersionPointers {
  std::string last;
  std::string current;
  std::string next;
};

struct SourceSpec {
  std::string type;
  std::string url;
  std::string sha256;
};

struct BuildSpec {
  std::string system = "make";
};

struct ScriptSpec {
  std::string patch;
  std::string build;
  std::string install;
  std::string check;
};

struct PortRecipe {
  std::string name;
  std::string version;
  std::string summary;
  std::string license;
  std::string install_mode = "store";
  std::vector<std::string> deps;
  SourceSpec src;
  BuildSpec build;
  ScriptSpec scripts;
  std::filesystem::path recipe_path;
};

class PortStore {
 public:
  static Result<VersionPointers> loadVersions(const std::filesystem::path& root,
                                              const Config& config,
                                              std::string_view port_name);
  static Result<PortRecipe> loadCurrentRecipe(const std::filesystem::path& root,
                                              const Config& config,
                                              std::string_view port_name);
  static Result<PortRecipe> loadRecipeAtVersion(const std::filesystem::path& root,
                                                const Config& config,
                                                std::string_view port_name,
                                                std::string_view version);
  static Status validateAll(const std::filesystem::path& root,
                            const Config& config);
};

}  // namespace pkg
