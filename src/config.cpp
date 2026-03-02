#include "pkg/config.hpp"

#include <filesystem>

#include "toml_util.hpp"

namespace pkg {

Result<Config> ConfigStore::load(const std::filesystem::path& root) {
  const auto path = root / kConfigFilename;
  auto parsed = toml_util::parseFile(path);
  if (!parsed.ok()) {
    return parsed.status();
  }

  Config cfg;
  const toml::Datum top = parsed.value().toptab();

  if (auto layout = top.get("layout"); layout.has_value() && layout->is_table()) {
    if (auto v = toml_util::getString(*layout, "ports_dir")) cfg.layout.ports_dir = *v;
    if (auto v = toml_util::getString(*layout, "groups_dir")) cfg.layout.groups_dir = *v;
    if (auto v = toml_util::getString(*layout, "store_dir")) cfg.layout.store_dir = *v;
    if (auto v = toml_util::getString(*layout, "build_dir")) cfg.layout.build_dir = *v;
    if (auto v = toml_util::getString(*layout, "profile_dir")) cfg.layout.profile_dir = *v;
    if (auto v = toml_util::getString(*layout, "current_profile")) cfg.layout.current_profile = *v;
    if (auto v = toml_util::getString(*layout, "lockfile")) cfg.layout.lockfile = *v;
  }

  if (auto resolver = top.get("resolver"); resolver.has_value() && resolver->is_table()) {
    if (auto v = toml_util::getString(*resolver, "strategy")) cfg.resolver.strategy = *v;
  }

  if (auto build = top.get("build"); build.has_value() && build->is_table()) {
    if (auto v = toml_util::getInt(*build, "jobs")) cfg.build.jobs = *v;
    if (auto v = build->get("keep_build_dirs"); v.has_value()) {
      if (auto b = v->as_bool()) cfg.build.keep_build_dirs = *b;
    }
    if (auto v = build->get("keep_failed_build_dirs"); v.has_value()) {
      if (auto b = v->as_bool()) cfg.build.keep_failed_build_dirs = *b;
    }
  }

  if (auto profile = top.get("profile"); profile.has_value() && profile->is_table()) {
    if (auto v = toml_util::getString(*profile, "activate_symlink")) cfg.profile.activate_symlink = *v;
    if (auto v = toml_util::getString(*profile, "activate_target")) cfg.profile.activate_target = *v;
    if (auto v = toml_util::getInt(*profile, "generations_to_keep")) cfg.profile.generations_to_keep = *v;
  }

  return cfg;
}

Status ConfigStore::validate(const std::filesystem::path& root) {
  auto loaded = load(root);
  if (!loaded.ok()) {
    return loaded.status();
  }

  const auto& cfg = loaded.value();
  const auto layout_status = toml_util::requireNonEmpty(cfg.layout.ports_dir, "layout.ports_dir", root / kConfigFilename);
  if (!layout_status.ok()) {
    return layout_status;
  }

  if (cfg.resolver.strategy != "strict") {
    return Status{StatusCode::kInvalidArgument,
                  "Only resolver.strategy='strict' is currently supported"};
  }

  return Status::Ok();
}

}  // namespace pkg
