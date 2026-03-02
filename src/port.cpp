#include "pkg/port.hpp"

#include <filesystem>
#include <string>

#include "toml_util.hpp"

namespace pkg {
namespace {

Status validateScriptPath(const std::filesystem::path& recipe_path,
                          const std::string& script,
                          std::string_view field_name,
                          bool required) {
  if (script.empty()) {
    if (required) {
      return Status{StatusCode::kParseError,
                    "Missing required scripts." + std::string(field_name) +
                        " in " + recipe_path.string()};
    }
    return Status::Ok();
  }
  const auto script_path = recipe_path.parent_path() / script;
  if (!std::filesystem::exists(script_path)) {
    return Status{StatusCode::kNotFound,
                  "Script not found for scripts." + std::string(field_name) +
                      ": " + script_path.string()};
  }
  if (!std::filesystem::is_regular_file(script_path)) {
    return Status{StatusCode::kInvalidArgument,
                  "Script path is not a file for scripts." + std::string(field_name) +
                      ": " + script_path.string()};
  }
  return Status::Ok();
}

Result<VersionPointers> loadVersionsFromPath(const std::filesystem::path& path) {
  auto parsed = toml_util::parseFile(path);
  if (!parsed.ok()) {
    return parsed.status();
  }
  VersionPointers pointers;
  const toml::Datum top = parsed.value().toptab();
  pointers.last = toml_util::getString(top, "last").value_or(std::string{});
  pointers.current = toml_util::getString(top, "current").value_or(std::string{});
  pointers.next = toml_util::getString(top, "next").value_or(std::string{});

  const auto required = toml_util::requireNonEmpty(pointers.current, "current", path);
  if (!required.ok()) {
    return required;
  }

  return pointers;
}

Result<PortRecipe> loadRecipeFromPath(const std::filesystem::path& path,
                                      std::string_view expected_name,
                                      std::string_view expected_version) {
  auto parsed = toml_util::parseFile(path);
  if (!parsed.ok()) {
    return parsed.status();
  }

  PortRecipe recipe;
  const toml::Datum top = parsed.value().toptab();

  recipe.name = toml_util::getString(top, "name").value_or(std::string{});
  recipe.version = toml_util::getString(top, "version").value_or(std::string{});
  recipe.summary = toml_util::getString(top, "summary").value_or(std::string{});
  recipe.license = toml_util::getString(top, "license").value_or(std::string{});
  recipe.install_mode = toml_util::getString(top, "install_mode").value_or("store");
  recipe.recipe_path = path;

  auto deps = toml_util::getStringArray(top, "deps");
  if (!deps.ok()) {
    return Status{deps.status().code(), deps.status().message() + " in " + path.string()};
  }
  recipe.deps = std::move(deps.value());

  if (auto src = top.get("src"); src.has_value() && src->is_table()) {
    recipe.src.type = toml_util::getString(*src, "type").value_or(std::string{});
    recipe.src.url = toml_util::getString(*src, "url").value_or(std::string{});
    recipe.src.sha256 = toml_util::getString(*src, "sha256").value_or(std::string{});
  }

  if (auto build = top.get("build"); build.has_value() && build->is_table()) {
    recipe.build.system = toml_util::getString(*build, "system").value_or("make");
  }
  if (auto scripts = top.get("scripts"); scripts.has_value() && scripts->is_table()) {
    recipe.scripts.patch = toml_util::getString(*scripts, "patch").value_or(std::string{});
    recipe.scripts.build = toml_util::getString(*scripts, "build").value_or(std::string{});
    recipe.scripts.install = toml_util::getString(*scripts, "install").value_or(std::string{});
    recipe.scripts.check = toml_util::getString(*scripts, "check").value_or(std::string{});
  }

  auto status = toml_util::requireNonEmpty(recipe.name, "name", path);
  if (!status.ok()) return status;
  status = toml_util::requireNonEmpty(recipe.version, "version", path);
  if (!status.ok()) return status;

  if (recipe.name != expected_name) {
    return Status{StatusCode::kParseError,
                  "Recipe name mismatch in " + path.string() + ": expected '" +
                      std::string(expected_name) + "' got '" + recipe.name + "'"};
  }
  if (recipe.version != expected_version) {
    return Status{StatusCode::kParseError,
                  "Recipe version mismatch in " + path.string() + ": expected '" +
                      std::string(expected_version) + "' got '" + recipe.version + "'"};
  }

  if (recipe.build.system != "make" && recipe.build.system != "gmake" &&
      recipe.build.system != "cmake" && recipe.build.system != "meson") {
    return Status{StatusCode::kInvalidArgument,
                  "Unsupported build.system in " + path.string() + ": " + recipe.build.system};
  }
  if (recipe.install_mode != "store" && recipe.install_mode != "system") {
    return Status{StatusCode::kInvalidArgument,
                  "Unsupported install_mode in " + path.string() + ": " + recipe.install_mode};
  }

  auto script_status =
      validateScriptPath(path, recipe.scripts.build, "build", true);
  if (!script_status.ok()) {
    return script_status;
  }
  script_status =
      validateScriptPath(path, recipe.scripts.install, "install", true);
  if (!script_status.ok()) {
    return script_status;
  }
  script_status =
      validateScriptPath(path, recipe.scripts.patch, "patch", false);
  if (!script_status.ok()) {
    return script_status;
  }
  script_status =
      validateScriptPath(path, recipe.scripts.check, "check", false);
  if (!script_status.ok()) {
    return script_status;
  }

  return recipe;
}

}  // namespace

Result<VersionPointers> PortStore::loadVersions(const std::filesystem::path& root,
                                                 const Config& config,
                                                 std::string_view port_name) {
  const auto path = root / config.layout.ports_dir / std::string(port_name) / "versions.toml";
  if (!std::filesystem::exists(path)) {
    return Status{StatusCode::kNotFound,
                  "versions.toml not found for port '" + std::string(port_name) + "'"};
  }
  return loadVersionsFromPath(path);
}

Result<PortRecipe> PortStore::loadCurrentRecipe(const std::filesystem::path& root,
                                                 const Config& config,
                                                 std::string_view port_name) {
  auto versions = loadVersions(root, config, port_name);
  if (!versions.ok()) {
    return versions.status();
  }
  return loadRecipeAtVersion(root, config, port_name, versions.value().current);
}

Result<PortRecipe> PortStore::loadRecipeAtVersion(const std::filesystem::path& root,
                                                  const Config& config,
                                                  std::string_view port_name,
                                                  std::string_view version) {
  const auto path = root / config.layout.ports_dir / std::string(port_name) /
                    std::string(version) / "pkg.toml";
  if (!std::filesystem::exists(path)) {
    return Status{StatusCode::kNotFound,
                  "pkg.toml not found: " + path.string()};
  }
  return loadRecipeFromPath(path, port_name, version);
}

Status PortStore::validateAll(const std::filesystem::path& root,
                              const Config& config) {
  const auto ports_dir = root / config.layout.ports_dir;
  if (!std::filesystem::exists(ports_dir)) {
    return Status{StatusCode::kNotFound,
                  "Ports directory not found: " + ports_dir.string()};
  }

  for (const auto& port_dir : std::filesystem::directory_iterator(ports_dir)) {
    if (!port_dir.is_directory()) {
      continue;
    }
    const std::string port_name = port_dir.path().filename().string();
    if (!std::filesystem::exists(port_dir.path() / "versions.toml")) {
      continue;
    }
    auto versions = loadVersions(root, config, port_name);
    if (!versions.ok()) {
      return versions.status();
    }
    auto recipe = loadRecipeAtVersion(root, config, port_name, versions.value().current);
    if (!recipe.ok()) {
      return recipe.status();
    }
  }
  return Status::Ok();
}

}  // namespace pkg
