#include "pkg/lockfile.hpp"

#include <filesystem>
#include <fstream>

#include "toml_util.hpp"

namespace pkg {

Result<Lockfile> LockfileStore::load(const std::filesystem::path& root,
                                     const Config& config) {
  const auto path = root / config.layout.lockfile;
  if (!std::filesystem::exists(path)) {
    return Status{StatusCode::kNotFound,
                  "Lockfile not found: " + path.string()};
  }

  auto parsed = toml_util::parseFile(path);
  if (!parsed.ok()) {
    return parsed.status();
  }

  Lockfile lock;
  const toml::Datum top = parsed.value().toptab();
  if (auto schema = toml_util::getInt(top, "schema")) {
    lock.schema = *schema;
  }
  lock.state = toml_util::getString(top, "state").value_or(std::string{});
  if (auto run = top.get("run"); run.has_value() && run->is_table()) {
    lock.run_command = toml_util::getString(*run, "command").value_or(std::string{});
    if (auto v = toml_util::getInt(*run, "started_unix")) lock.started_unix = *v;
    if (auto v = toml_util::getInt(*run, "finished_unix")) lock.finished_unix = *v;
  }
  if (auto summary = top.get("summary"); summary.has_value() && summary->is_table()) {
    if (auto v = toml_util::getInt(*summary, "planned")) lock.summary_planned = *v;
    if (auto v = toml_util::getInt(*summary, "built")) lock.summary_built = *v;
    if (auto v = toml_util::getInt(*summary, "reused")) lock.summary_reused = *v;
    if (auto v = toml_util::getInt(*summary, "failed")) lock.summary_failed = *v;
    if (auto v = toml_util::getInt(*summary, "skipped")) lock.summary_skipped = *v;
  }

  auto entry_arr = top.get("entry");
  if (entry_arr.has_value() && entry_arr->is_array()) {
    auto rows = entry_arr->as_vector();
    if (!rows.has_value()) {
      return Status{StatusCode::kParseError, "Invalid [[entry]] array in lockfile"};
    }
    for (const auto& row : *rows) {
      if (!row.is_table()) {
        continue;
      }
      LockEntry e;
      e.name = toml_util::getString(row, "name").value_or(std::string{});
      e.version = toml_util::getString(row, "version").value_or(std::string{});
      e.status = toml_util::getString(row, "status").value_or(std::string{});
      e.recipe = toml_util::getString(row, "recipe").value_or(std::string{});
      e.store = toml_util::getString(row, "store").value_or(std::string{});
      e.log = toml_util::getString(row, "log").value_or(std::string{});
      e.error = toml_util::getString(row, "error").value_or(std::string{});
      if (auto v = toml_util::getInt(row, "build_seconds")) {
        e.build_seconds = *v;
      }
      auto deps = toml_util::getStringArray(row, "deps");
      if (!deps.ok()) {
        return deps.status();
      }
      e.deps = std::move(deps.value());
      lock.entries.push_back(std::move(e));
    }
  }

  return lock;
}

Status LockfileStore::save(const std::filesystem::path& root,
                           const Config& config,
                           const Lockfile& lockfile) {
  const auto path = root / config.layout.lockfile;
  std::ofstream out(path);
  if (!out) {
    return Status{StatusCode::kIoError,
                  "Failed to open lockfile for write: " + path.string()};
  }

  out << "schema = " << lockfile.schema << "\n";
  out << "state = \"" << lockfile.state << "\"\n\n";
  out << "[run]\n";
  out << "command = \"" << lockfile.run_command << "\"\n";
  out << "started_unix = " << lockfile.started_unix << "\n";
  out << "finished_unix = " << lockfile.finished_unix << "\n\n";
  out << "[summary]\n";
  out << "planned = " << lockfile.summary_planned << "\n";
  out << "built = " << lockfile.summary_built << "\n";
  out << "reused = " << lockfile.summary_reused << "\n";
  out << "failed = " << lockfile.summary_failed << "\n";
  out << "skipped = " << lockfile.summary_skipped << "\n\n";

  for (const auto& e : lockfile.entries) {
    out << "[[entry]]\n";
    out << "name = \"" << e.name << "\"\n";
    out << "version = \"" << e.version << "\"\n";
    out << "status = \"" << e.status << "\"\n";
    out << "recipe = \"" << e.recipe << "\"\n";
    out << "store = \"" << e.store << "\"\n";
    if (!e.log.empty()) {
      out << "log = \"" << e.log << "\"\n";
    }
    if (!e.error.empty()) {
      out << "error = \"" << e.error << "\"\n";
    }
    if (e.build_seconds > 0) {
      out << "build_seconds = " << e.build_seconds << "\n";
    }
    out << "deps = [";
    for (size_t i = 0; i < e.deps.size(); ++i) {
      out << "\"" << e.deps[i] << "\"";
      if (i + 1 != e.deps.size()) {
        out << ", ";
      }
    }
    out << "]\n\n";
  }

  if (!out.good()) {
    return Status{StatusCode::kIoError,
                  "Failed while writing lockfile: " + path.string()};
  }

  return Status::Ok();
}

}  // namespace pkg
