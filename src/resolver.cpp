#include "pkg/resolver.hpp"

#include <unordered_set>

#include "pkg/port.hpp"

namespace pkg {
namespace {

Status dfsResolve(const std::filesystem::path& root,
                  const Config& config,
                  std::string_view port_name,
                  std::unordered_map<std::string, int>& visit_state,
                  std::unordered_map<std::string, std::string>& selected_versions,
                  ResolveResult& out) {
  const std::string key(port_name);
  const int state = visit_state[key];
  if (state == 1) {
    return Status{StatusCode::kConflict,
                  "Dependency cycle detected at port: " + key};
  }
  if (state == 2) {
    return Status::Ok();
  }

  auto recipe_result = PortStore::loadCurrentRecipe(root, config, key);
  if (!recipe_result.ok()) {
    return recipe_result.status();
  }

  const PortRecipe& recipe = recipe_result.value();
  auto it = selected_versions.find(recipe.name);
  if (it != selected_versions.end() && it->second != recipe.version) {
    return Status{StatusCode::kConflict,
                  "Version conflict for port '" + recipe.name + "': '" +
                      it->second + "' vs '" + recipe.version + "'"};
  }
  selected_versions[recipe.name] = recipe.version;

  visit_state[key] = 1;

  for (const std::string& dep : recipe.deps) {
    auto dep_status = dfsResolve(root, config, dep, visit_state, selected_versions, out);
    if (!dep_status.ok()) {
      return dep_status;
    }
  }

  visit_state[key] = 2;

  if (out.nodes.find(recipe.name) == out.nodes.end()) {
    out.order.push_back(recipe.name);
    out.nodes.emplace(recipe.name, ResolvedNode{recipe});
  }

  return Status::Ok();
}

}  // namespace

Result<ResolveResult> Resolver::resolveGroup(const std::filesystem::path& root,
                                             const Config& config,
                                             const Group& group) {
  ResolveResult out;
  std::unordered_map<std::string, int> visit_state;
  std::unordered_map<std::string, std::string> selected_versions;

  for (const std::string& root_port : group.ports) {
    auto status = dfsResolve(root, config, root_port, visit_state, selected_versions, out);
    if (!status.ok()) {
      return status;
    }
  }

  return out;
}

}  // namespace pkg
