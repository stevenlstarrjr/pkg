#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "pkg/config.hpp"
#include "pkg/result.hpp"

namespace pkg {

struct Group {
  std::string name;
  std::string summary;
  std::vector<std::string> ports;
};

class GroupStore {
 public:
  static Result<Group> loadByName(const std::filesystem::path& root,
                                  const Config& config,
                                  std::string_view group_name);
  static Status validateAll(const std::filesystem::path& root,
                            const Config& config);
};

}  // namespace pkg
