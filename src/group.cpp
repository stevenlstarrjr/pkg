#include "pkg/group.hpp"

#include <filesystem>
#include <vector>

#include "toml_util.hpp"

namespace pkg {
namespace {

Result<Group> loadGroupFromPath(const std::filesystem::path& path) {
  auto parsed = toml_util::parseFile(path);
  if (!parsed.ok()) {
    return parsed.status();
  }

  Group group;
  const toml::Datum top = parsed.value().toptab();
  group.name = toml_util::getString(top, "name").value_or(path.stem().string());
  group.summary = toml_util::getString(top, "summary").value_or(std::string{});

  auto ports = toml_util::getStringArray(top, "ports");
  if (!ports.ok()) {
    return Status{ports.status().code(),
                  ports.status().message() + " in " + path.string()};
  }
  group.ports = std::move(ports.value());
  if (group.ports.empty()) {
    return Status{StatusCode::kParseError,
                  "Group has no ports: " + path.string()};
  }
  return group;
}

std::vector<std::filesystem::path> candidateGroupDirs(
    const std::filesystem::path& root,
    const Config& config) {
  std::vector<std::filesystem::path> dirs;
  auto push_unique = [&](const std::filesystem::path& p) {
    for (const auto& d : dirs) {
      if (d == p) {
        return;
      }
    }
    dirs.push_back(p);
  };
  push_unique(root / config.layout.groups_dir);
  push_unique(root / "groups");
  push_unique(root / "group");
  return dirs;
}

}  // namespace

Result<Group> GroupStore::loadByName(const std::filesystem::path& root,
                                     const Config& config,
                                     std::string_view group_name) {
  std::string searched;
  for (const auto& dir : candidateGroupDirs(root, config)) {
    const auto path = dir / (std::string(group_name) + ".toml");
    if (std::filesystem::exists(path)) {
      return loadGroupFromPath(path);
    }
    if (!searched.empty()) {
      searched += ", ";
    }
    searched += path.string();
  }
  return Status{StatusCode::kNotFound,
                "Group file not found. searched: " + searched};
}

Status GroupStore::validateAll(const std::filesystem::path& root,
                               const Config& config) {
  bool found_any_dir = false;
  for (const auto& groups_dir : candidateGroupDirs(root, config)) {
    if (!std::filesystem::exists(groups_dir) ||
        !std::filesystem::is_directory(groups_dir)) {
      continue;
    }
    found_any_dir = true;
    for (const auto& entry : std::filesystem::directory_iterator(groups_dir)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".toml") {
        continue;
      }
      auto group = loadGroupFromPath(entry.path());
      if (!group.ok()) {
        return group.status();
      }
    }
  }
  if (!found_any_dir) {
    return Status{StatusCode::kNotFound,
                  "No group directory found (checked: " +
                      (root / config.layout.groups_dir).string() + ", " +
                      (root / "groups").string() + ", " +
                      (root / "group").string() + ")"};
  }
  return Status::Ok();
}

}  // namespace pkg
