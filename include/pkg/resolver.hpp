#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "pkg/config.hpp"
#include "pkg/group.hpp"
#include "pkg/port.hpp"
#include "pkg/result.hpp"

namespace pkg {

struct ResolvedNode {
  PortRecipe recipe;
};

struct ResolveResult {
  std::vector<std::string> order;
  std::unordered_map<std::string, ResolvedNode> nodes;
};

class Resolver {
 public:
  static Result<ResolveResult> resolveGroup(const std::filesystem::path& root,
                                            const Config& config,
                                            const Group& group);
};

}  // namespace pkg
