#include "pkg/commands.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "pkg/config.hpp"
#include "pkg/group.hpp"
#include "pkg/lockfile.hpp"
#include "pkg/port.hpp"
#include "pkg/resolver.hpp"

namespace pkg {
namespace {

int runApply(const std::filesystem::path& root,
             const std::vector<std::string>& args);

void printUsage() {
  std::cout
      << "Usage:\n"
      << "  pkg validate [--root <path>]\n"
      << "  pkg resolve --group <name> [--root <path>]\n"
      << "  pkg resolve <port> [<port> ...] [--root <path>]\n"
      << "  pkg build --group <name> [--root <path>]\n"
      << "  pkg build <port> [<port> ...] [--root <path>] [--activate]\n"
      << "  pkg package --group <name> [--root <path>] [--out <path>]\n"
      << "  pkg package <port> [<port> ...] [--root <path>] [--out <path>]\n"
      << "  pkg download --group <name> [--root <path>] [--activate]\n"
      << "  pkg download <port> [<port> ...] [--root <path>] [--activate]\n"
      << "  pkg activate --group <name> [--root <path>]\n"
      << "  pkg activate --profile <name-or-path> [--root <path>]\n"
      << "  pkg apply [--root <path>] [--group <name>] [--profile <name-or-path>] "
         "[--activate-target <path>] [--force]\n";
}

void printStatusError(const Status& status) {
  std::cerr << "error: " << status.message() << "\n";
}

std::string shellQuote(const std::string& value) {
  std::string out = "'";
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

std::string hashKey(std::string_view text) {
  const std::size_t h = std::hash<std::string_view>{}(text);
  std::ostringstream oss;
  oss << std::hex << std::setfill('0') << std::setw(16)
      << static_cast<unsigned long long>(h);
  return oss.str();
}

bool shouldSkipProfileEntry(const std::filesystem::path& rel) {
  const auto rel_text = rel.generic_string();
  return rel_text == "usr/share/info/dir" ||
         rel_text == "etc/ld.so.cache";
}

bool sameFileContents(const std::filesystem::path& lhs,
                      const std::filesystem::path& rhs) {
  std::error_code ec;
  if (std::filesystem::file_size(lhs, ec) != std::filesystem::file_size(rhs, ec)) {
    return false;
  }

  std::ifstream a(lhs, std::ios::binary);
  std::ifstream b(rhs, std::ios::binary);
  if (!a || !b) {
    return false;
  }

  constexpr std::streamsize kBufSize = 8192;
  char buf_a[kBufSize];
  char buf_b[kBufSize];
  while (a && b) {
    a.read(buf_a, kBufSize);
    b.read(buf_b, kBufSize);
    const auto count_a = a.gcount();
    const auto count_b = b.gcount();
    if (count_a != count_b) {
      return false;
    }
    if (count_a == 0) {
      break;
    }
    if (!std::equal(buf_a, buf_a + count_a, buf_b)) {
      return false;
    }
  }
  return true;
}

bool sameProfileSource(const std::filesystem::path& lhs,
                       const std::filesystem::path& rhs) {
  std::error_code ec;
  const bool lhs_is_symlink = std::filesystem::is_symlink(lhs, ec);
  ec.clear();
  const bool rhs_is_symlink = std::filesystem::is_symlink(rhs, ec);
  ec.clear();
  if (lhs_is_symlink || rhs_is_symlink) {
    if (!(lhs_is_symlink && rhs_is_symlink)) {
      return false;
    }
    return std::filesystem::read_symlink(lhs, ec) ==
           std::filesystem::read_symlink(rhs, ec);
  }
  return sameFileContents(lhs, rhs);
}

std::filesystem::path repoFilesystemRoot(const std::filesystem::path& root) {
  const auto abs_root = std::filesystem::absolute(root);
  auto fs_root = abs_root;
  if (fs_root.has_parent_path()) {
    fs_root = fs_root.parent_path();
  }
  if (fs_root.has_parent_path()) {
    fs_root = fs_root.parent_path();
  }
  return fs_root;
}

std::filesystem::path resolveInTargetRoot(const std::filesystem::path& root,
                                          const std::filesystem::path& path) {
  const auto fs_root = repoFilesystemRoot(root);
  if (path.is_absolute()) {
    return fs_root / path.relative_path();
  }
  return std::filesystem::absolute(root / path);
}

std::filesystem::path logicalJoin(const std::filesystem::path& base,
                                  const std::filesystem::path& rel) {
  if (base.is_absolute()) {
    return std::filesystem::path("/") / base.relative_path() / rel;
  }
  return base / rel;
}

Status runCommandToLog(const std::string& command,
                       const std::filesystem::path& log_path,
                       bool stream_to_terminal = false) {
  std::string shell_cmd;
  if (stream_to_terminal) {
    const std::string pipeline =
        "set -o pipefail; ( " + command + " ) 2>&1 | tee -a " +
        shellQuote(log_path.string());
    shell_cmd = "/bin/bash -lc " + shellQuote(pipeline);
  } else {
    shell_cmd = command + " >> " + shellQuote(log_path.string()) + " 2>&1";
  }
  const int rc = std::system(shell_cmd.c_str());
  if (rc != 0) {
    return Status{StatusCode::kInternalError,
                  "Command failed: " + command + " (see " +
                      log_path.string() + ")"};
  }
  return Status::Ok();
}

Status prepareSource(const std::filesystem::path& root,
                     const PortRecipe& recipe,
                     const std::filesystem::path& src_dir,
                     const std::filesystem::path& downloads_dir,
                     const std::filesystem::path& log_path) {
  std::error_code ec;
  std::filesystem::create_directories(src_dir, ec);
  if (ec) {
    return Status{StatusCode::kIoError,
                  "Failed to create source dir: " + src_dir.string()};
  }

  if (recipe.src.type == "git") {
    if (recipe.src.url.empty()) {
      return Status::Ok();
    }
    const bool use_head =
        recipe.version.empty() || recipe.version == "main" ||
        recipe.version == "master" || recipe.version == "head" ||
        recipe.version == "latest";
    const auto git_dir = src_dir / ".git";
    if (!std::filesystem::exists(git_dir)) {
      if (!std::filesystem::is_empty(src_dir)) {
        std::filesystem::remove_all(src_dir, ec);
        ec.clear();
        std::filesystem::create_directories(src_dir, ec);
        if (ec) {
          return Status{StatusCode::kIoError,
                        "Failed to reset source dir for git clone: " +
                            src_dir.string()};
        }
      }
      if (use_head) {
        return runCommandToLog(
            "git clone --depth 1 " + shellQuote(recipe.src.url) + " " +
                shellQuote(src_dir.string()),
            log_path);
      }
      auto s = runCommandToLog(
          "git clone --depth 1 --branch " + shellQuote(recipe.version) + " " +
              shellQuote(recipe.src.url) + " " + shellQuote(src_dir.string()) +
              " || git clone --depth 1 " + shellQuote(recipe.src.url) + " " +
                  shellQuote(src_dir.string()),
          log_path);
      if (!s.ok()) {
        return s;
      }
      const std::string tag_ref = "refs/tags/" + recipe.version;
      return runCommandToLog(
          "git -C " + shellQuote(src_dir.string()) + " rev-parse --verify " +
              shellQuote(tag_ref) +
              " >/dev/null 2>&1 && git -C " + shellQuote(src_dir.string()) +
              " checkout -q " + shellQuote(tag_ref) + " || git -C " +
              shellQuote(src_dir.string()) + " checkout -q " +
              shellQuote(recipe.version),
          log_path);
    }
    if (use_head) {
      auto s = runCommandToLog("git -C " + shellQuote(src_dir.string()) +
                                   " fetch --depth 1 origin",
                               log_path);
      if (!s.ok()) {
        return s;
      }
      return runCommandToLog("git -C " + shellQuote(src_dir.string()) +
                                 " reset --hard origin/HEAD",
                             log_path);
    }
    auto s = runCommandToLog("git -C " + shellQuote(src_dir.string()) +
                                 " fetch --depth 1 origin " +
                                 shellQuote(recipe.version) + " || true",
                             log_path);
    if (!s.ok()) {
      return s;
    }
    s = runCommandToLog("git -C " + shellQuote(src_dir.string()) +
                            " fetch --depth 1 --tags origin || true",
                        log_path);
    if (!s.ok()) {
      return s;
    }
    const std::string tag_ref = "refs/tags/" + recipe.version;
    return runCommandToLog("git -C " + shellQuote(src_dir.string()) +
                               " rev-parse --verify " + shellQuote(tag_ref) +
                               " >/dev/null 2>&1 && git -C " +
                               shellQuote(src_dir.string()) + " checkout -q " +
                               shellQuote(tag_ref) + " || git -C " +
                               shellQuote(src_dir.string()) +
                               " checkout -q " + shellQuote(recipe.version),
                           log_path);
  }

  if (recipe.src.type == "url") {
    if (recipe.src.url.empty()) {
      return Status::Ok();
    }
    std::filesystem::create_directories(downloads_dir, ec);
    if (ec) {
      return Status{StatusCode::kIoError,
                    "Failed to create downloads dir: " + downloads_dir.string()};
    }
    std::string filename = "source.tar";
    const auto slash = recipe.src.url.find_last_of('/');
    if (slash != std::string::npos && slash + 1 < recipe.src.url.size()) {
      filename = recipe.src.url.substr(slash + 1);
    }
    const auto archive_path =
        downloads_dir / (recipe.name + "-" + recipe.version + "-" + filename);

    auto verifyArchive = [&]() -> Status {
      if (!std::filesystem::exists(archive_path)) {
        return Status{StatusCode::kNotFound,
                      "archive missing: " + archive_path.string()};
      }
      if (recipe.src.sha256.empty()) {
        return Status::Ok();
      }
      auto s = runCommandToLog(
          "(command -v sha256 >/dev/null 2>&1 && "
          "test \"$(sha256 -q " + shellQuote(archive_path.string()) +
              ")\" = " + shellQuote(recipe.src.sha256) + ") || "
          "(command -v sha256sum >/dev/null 2>&1 && "
          "set -- $(sha256sum " + shellQuote(archive_path.string()) +
              "); test \"$1\" = " + shellQuote(recipe.src.sha256) + ")",
          log_path);
      if (!s.ok()) {
        return Status{StatusCode::kInternalError,
                      "sha256 mismatch for " + archive_path.string()};
      }
      return Status::Ok();
    };

    auto s = verifyArchive();
    if (!s.ok()) {
      if (s.code() != StatusCode::kNotFound) {
        return s;
      }

      std::filesystem::path ca_bundle;
      const auto ca_certificates = root / "ca-certificates.crt";
      const auto ca_bundle_fallback = root / "ca-bundle.crt";
      if (std::filesystem::exists(ca_certificates)) {
        ca_bundle = ca_certificates;
      } else if (std::filesystem::exists(ca_bundle_fallback)) {
        ca_bundle = ca_bundle_fallback;
      }

      std::string fetch_tool_cmd;
      if (!ca_bundle.empty()) {
        fetch_tool_cmd =
            "(command -v fetch >/dev/null 2>&1 && SSL_CERT_FILE=" +
            shellQuote(ca_bundle.string()) + " fetch -o " +
            shellQuote(archive_path.string()) + " " +
            shellQuote(recipe.src.url) +
            ") || (command -v curl >/dev/null 2>&1 && curl --cacert " +
            shellQuote(ca_bundle.string()) + " -LfsS -o " +
            shellQuote(archive_path.string()) + " " +
            shellQuote(recipe.src.url) + ")";
      } else {
        fetch_tool_cmd =
            "(command -v fetch >/dev/null 2>&1 && fetch -o " +
            shellQuote(archive_path.string()) + " " +
            shellQuote(recipe.src.url) +
            ") || (command -v curl >/dev/null 2>&1 && curl -LfsS -o " +
            shellQuote(archive_path.string()) + " " +
            shellQuote(recipe.src.url) + ")";
      }
      const std::string fetch_cmd = fetch_tool_cmd;
      s = runCommandToLog(fetch_cmd, log_path);
      if (!s.ok()) {
        return s;
      }

      s = verifyArchive();
      if (!s.ok()) {
        return s;
      }
    }

    std::filesystem::remove_all(src_dir, ec);
    ec.clear();
    std::filesystem::create_directories(src_dir, ec);
    if (ec) {
      return Status{StatusCode::kIoError,
                    "Failed to prepare source dir: " + src_dir.string()};
    }

    s = runCommandToLog(
        "tar -xf " + shellQuote(archive_path.string()) + " -C " +
            shellQuote(src_dir.string()) +
            " --no-same-owner --no-same-permissions --strip-components=1 || "
            "tar -xf " + shellQuote(archive_path.string()) + " -C " +
            shellQuote(src_dir.string()) +
            " --no-same-owner --no-same-permissions",
        log_path);
    if (!s.ok()) {
      return s;
    }
    return Status::Ok();
  }

  if (recipe.src.type.empty()) {
    return Status::Ok();
  }

  return Status{StatusCode::kInvalidArgument,
                "Unsupported src.type for " + recipe.name + ": " +
                    recipe.src.type};
}

Status runScript(const std::filesystem::path& script_path,
                 const std::filesystem::path& log_path,
                 const PortRecipe& recipe,
                 const std::filesystem::path& root,
                 const std::filesystem::path& src_dir,
                 const std::filesystem::path& build_dir,
                 const std::filesystem::path& store_dir,
                 const std::string& extra_path_prefix,
                 int jobs) {
  const char* current_path = std::getenv("PATH");
  std::string merged_path = extra_path_prefix;
  if (current_path != nullptr && *current_path != '\0') {
    if (!merged_path.empty()) {
      merged_path += ":";
    }
    merged_path += current_path;
  }

  std::ostringstream cmd;
  cmd << "env "
      << "PKG_NAME=" << shellQuote(recipe.name) << " "
      << "PKG_VERSION=" << shellQuote(recipe.version) << " "
      << "PKG_ROOT=" << shellQuote(root.string()) << " "
      << "PKG_SRC_DIR=" << shellQuote(src_dir.string()) << " "
      << "PKG_BUILD_DIR=" << shellQuote(build_dir.string()) << " "
      << "PKG_STORE_DIR=" << shellQuote(store_dir.string()) << " "
      << "PKG_JOBS=" << shellQuote(std::to_string(jobs)) << " "
      << "PATH=" << shellQuote(merged_path) << " "
      << "/bin/sh " << shellQuote(script_path.string());

  return runCommandToLog(cmd.str(), log_path, true);
}

Status validateInstallScriptSafety(const PortRecipe& recipe,
                                   const std::filesystem::path& install_script_path) {
  if (recipe.install_mode != "store") {
    return Status::Ok();
  }

  std::ifstream in(install_script_path);
  if (!in) {
    return Status{StatusCode::kNotFound,
                  "Install script not found: " + install_script_path.string()};
  }

  // Reject plain `make install`-style commands for store mode.
  const std::regex plain_make_install(
      R"(^[[:space:]]*make([[:space:]][^#]*)?[[:space:]]install([[:space:]]|$))");
  const std::regex has_staging_var(R"(DESTDIR=|install_root=|--destdir)");
  std::string line;
  int line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    if (std::regex_search(line, plain_make_install) &&
        !std::regex_search(line, has_staging_var)) {
      return Status{
          StatusCode::kInvalidArgument,
          "Unsafe install in store mode at " + install_script_path.string() +
              ":" + std::to_string(line_no) +
              " (plain make install). Use DESTDIR/install_root staging."};
    }
  }
  return Status::Ok();
}

Status materializeProfile(const std::filesystem::path& root,
                          const Config& cfg,
                          const Lockfile& lockfile,
                          const std::unordered_set<std::string>* allowed_names,
                          int* out_linked_files) {
  const auto current_profile = root / cfg.layout.current_profile;
  const auto staging_profile =
      (root / cfg.layout.profile_dir) / ".current-staging";

  std::error_code ec;
  std::filesystem::remove_all(staging_profile, ec);
  ec.clear();
  std::filesystem::create_directories(staging_profile, ec);
  if (ec) {
    return Status{StatusCode::kIoError,
                  "Failed to create staging profile: " +
                      staging_profile.string()};
  }

  std::unordered_map<std::string, std::filesystem::path> linked_targets;
  int linked_files = 0;
  for (const auto& entry : lockfile.entries) {
    if (entry.status != "built" && entry.status != "reused") {
      continue;
    }
    if (allowed_names != nullptr &&
        allowed_names->find(entry.name) == allowed_names->end()) {
      continue;
    }
    const auto store_dir = root / entry.store;
    if (!std::filesystem::exists(store_dir) ||
        !std::filesystem::is_directory(store_dir)) {
      return Status{StatusCode::kNotFound,
                    "Store path not found for " + entry.name + ": " +
                        store_dir.string()};
    }

    for (const auto& it : std::filesystem::recursive_directory_iterator(store_dir)) {
      const auto rel = it.path().lexically_relative(store_dir);
      const auto dst = staging_profile / rel;
      if (it.is_directory()) {
        std::filesystem::create_directories(dst, ec);
        if (ec) {
          return Status{StatusCode::kIoError,
                        "Failed creating profile directory: " + dst.string()};
        }
        continue;
      }
      if (!it.is_regular_file() && !it.is_symlink()) {
        continue;
      }
      if (shouldSkipProfileEntry(rel)) {
        continue;
      }

      std::filesystem::create_directories(dst.parent_path(), ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed creating parent directory: " +
                          dst.parent_path().string()};
      }

      const std::string rel_key = rel.string();
      auto existing = linked_targets.find(rel_key);
      if (existing != linked_targets.end() &&
          existing->second != it.path() &&
          !sameProfileSource(existing->second, it.path())) {
        return Status{StatusCode::kConflict,
                      "Profile collision at " + rel_key + " between " +
                          existing->second.string() + " and " +
                          it.path().string()};
      }
      if (existing != linked_targets.end()) {
        continue;
      }

      if (std::filesystem::exists(dst, ec) || std::filesystem::is_symlink(dst, ec)) {
        std::filesystem::remove(dst, ec);
      }
      ec.clear();
      std::filesystem::create_symlink(it.path(), dst, ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed linking profile file: " + dst.string()};
      }
      linked_targets.emplace(rel_key, it.path());
      ++linked_files;
    }
  }

  const auto profile_root = root / cfg.layout.profile_dir;
  const auto generations_dir = profile_root / "generations";

  std::filesystem::create_directories(current_profile.parent_path(), ec);
  if (ec) {
    return Status{StatusCode::kIoError,
                  "Failed to create profile parent: " +
                      current_profile.parent_path().string()};
  }

  if (std::filesystem::exists(current_profile)) {
    std::filesystem::create_directories(generations_dir, ec);
    if (ec) {
      return Status{StatusCode::kIoError,
                    "Failed creating generations dir: " +
                        generations_dir.string()};
    }
    const auto now = static_cast<long long>(std::time(nullptr));
    const auto generation_dir =
        generations_dir / ("gen-" + std::to_string(now));
    std::filesystem::rename(current_profile, generation_dir, ec);
    if (ec) {
      std::filesystem::remove_all(current_profile, ec);
      ec.clear();
    }
  }

  std::filesystem::rename(staging_profile, current_profile, ec);
  if (ec) {
    return Status{StatusCode::kIoError,
                  "Failed to activate profile: " + current_profile.string()};
  }

  if (cfg.profile.generations_to_keep >= 0 &&
      std::filesystem::exists(generations_dir)) {
    std::vector<std::filesystem::path> gens;
    for (const auto& e : std::filesystem::directory_iterator(generations_dir)) {
      if (e.is_directory()) {
        gens.push_back(e.path());
      }
    }
    std::sort(gens.begin(), gens.end());
    while (static_cast<int>(gens.size()) > cfg.profile.generations_to_keep) {
      std::filesystem::remove_all(gens.front(), ec);
      gens.erase(gens.begin());
    }
  }

  if (out_linked_files) {
    *out_linked_files = linked_files;
  }
  return Status::Ok();
}

Status projectActivatedProfile(const std::filesystem::path& root,
                               const std::filesystem::path& activation_target) {
  const auto activation_target_fs = resolveInTargetRoot(root, activation_target);
  if (!std::filesystem::exists(activation_target_fs) ||
      !std::filesystem::is_directory(activation_target_fs)) {
    return Status{StatusCode::kNotFound,
                  "Activation target not found: " + activation_target_fs.string()};
  }

  const auto fs_root = repoFilesystemRoot(root);
  std::error_code ec;
  int linked_files = 0;
  for (const auto& it :
       std::filesystem::recursive_directory_iterator(activation_target_fs)) {
    const auto rel = it.path().lexically_relative(activation_target_fs);
    if (rel.empty()) {
      continue;
    }
    const auto dst = fs_root / rel;
    if (it.is_directory()) {
      if (std::filesystem::exists(dst, ec) && !std::filesystem::is_directory(dst, ec)) {
        std::filesystem::remove_all(dst, ec);
        if (ec) {
          return Status{StatusCode::kIoError,
                        "Failed replacing non-directory path: " + dst.string()};
        }
      }
      std::filesystem::create_directories(dst, ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed creating activation directory: " + dst.string()};
      }
      continue;
    }
    if (!it.is_regular_file() && !it.is_symlink()) {
      continue;
    }

    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec) {
      return Status{StatusCode::kIoError,
                    "Failed creating activation parent: " +
                        dst.parent_path().string()};
    }

    const auto logical_target = logicalJoin(activation_target, rel);
    if (std::filesystem::is_symlink(dst, ec)) {
      ec.clear();
      if (std::filesystem::read_symlink(dst, ec) == logical_target) {
        continue;
      }
      ec.clear();
    }
    if (std::filesystem::exists(dst, ec) || std::filesystem::is_symlink(dst, ec)) {
      if (std::filesystem::is_directory(dst, ec) && !std::filesystem::is_symlink(dst, ec)) {
        return Status{StatusCode::kConflict,
                      "Activation would replace directory with file link: " +
                          dst.string()};
      }
      std::filesystem::remove(dst, ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed removing existing activation entry: " + dst.string()};
      }
    }
    ec.clear();
    std::filesystem::create_symlink(logical_target, dst, ec);
    if (ec) {
      return Status{StatusCode::kIoError,
                    "Failed creating activation symlink: " + dst.string()};
    }
    ++linked_files;
  }

  const auto ensure_link = [&](const std::filesystem::path& path,
                               const std::filesystem::path& target) -> Status {
    std::error_code link_ec;
    const auto path_fs = resolveInTargetRoot(root, path);
    if (std::filesystem::is_symlink(path_fs, link_ec)) {
      link_ec.clear();
      if (std::filesystem::read_symlink(path_fs, link_ec) == target) {
        return Status::Ok();
      }
      std::filesystem::remove(path_fs, link_ec);
      if (link_ec) {
        return Status{StatusCode::kIoError,
                      "Failed updating compatibility symlink: " +
                          path_fs.string()};
      }
    } else if (std::filesystem::exists(path_fs, link_ec)) {
      return Status::Ok();
    }
    std::filesystem::create_symlink(target, path_fs, link_ec);
    if (link_ec) {
      return Status{StatusCode::kIoError,
                    "Failed creating compatibility symlink: " + path_fs.string()};
    }
    return Status::Ok();
  };

  auto s = ensure_link("/bin", "usr/bin");
  if (!s.ok()) {
    return s;
  }
  s = ensure_link("/sbin", "usr/sbin");
  if (!s.ok()) {
    return s;
  }
  s = ensure_link("/lib", "usr/lib");
  if (!s.ok()) {
    return s;
  }

  (void)linked_files;
  return Status::Ok();
}

Status updateActivationSymlink(const std::filesystem::path& root,
                               const Config& cfg,
                               const std::filesystem::path& activation_target,
                               bool force_replace) {
  const std::filesystem::path activation_link =
      resolveInTargetRoot(root, cfg.profile.activate_symlink);
  std::error_code ec;

  const bool link_exists = std::filesystem::exists(activation_link, ec);
  if (ec) {
    ec.clear();
  }
  const bool link_is_symlink = std::filesystem::is_symlink(activation_link, ec);
  // is_symlink() on a missing path may set ENOENT; that's not an error here.
  if (ec && ec != std::errc::no_such_file_or_directory) {
    return Status{StatusCode::kIoError,
                  "Failed to inspect activation path " +
                      activation_link.string() + ": " + ec.message()};
  }
  ec.clear();

  // Handle both normal paths and dangling symlinks.
  if (link_exists || link_is_symlink) {
    if (!link_is_symlink) {
      if (!force_replace) {
        return Status{StatusCode::kIoError,
                      "Refusing to replace non-symlink activation path: " +
                          activation_link.string() +
                          " (use --force to replace it)"};
      }
      std::filesystem::remove_all(activation_link, ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed to remove activation path: " +
                          activation_link.string() + ": " + ec.message()};
      }
    } else {
      std::filesystem::remove(activation_link, ec);
      if (ec) {
        return Status{StatusCode::kIoError,
                      "Failed to remove existing activation symlink: " +
                          activation_link.string() + ": " + ec.message()};
      }
    }
  }

  std::filesystem::create_symlink(activation_target, activation_link, ec);
  if (ec) {
    if (ec == std::errc::function_not_supported ||
        ec == std::errc::not_supported ||
        ec == std::errc::operation_not_supported) {
      const std::string ln_cmd = "ln -sfn " + shellQuote(activation_target.string()) +
                                 " " + shellQuote(activation_link.string());
      if (std::system(ln_cmd.c_str()) == 0) {
        return Status::Ok();
      }
    }
    return Status{StatusCode::kIoError,
                  "Failed to update activation symlink " +
                      activation_link.string() + " -> " +
                      activation_target.string() + ": " + ec.message()};
  }
  return Status::Ok();
}

std::filesystem::path discoverRootFrom(const std::filesystem::path& start) {
  std::error_code ec;
  auto current = std::filesystem::absolute(start, ec);
  if (ec) {
    return start;
  }

  while (true) {
    if (std::filesystem::exists(current / ConfigStore::kConfigFilename, ec)) {
      return current;
    }
    const auto parent = current.parent_path();
    if (parent.empty() || parent == current) {
      break;
    }
    current = parent;
  }

  const std::filesystem::path default_root = "/usr/ports";
  if (std::filesystem::exists(default_root / ConfigStore::kConfigFilename, ec)) {
    return default_root;
  }

  return start;
}

std::filesystem::path parseRoot(const std::vector<std::string>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--root") {
      return args[i + 1];
    }
  }
  return discoverRootFrom(std::filesystem::current_path());
}

std::string parseGroup(const std::vector<std::string>& args) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == "--group") {
      return args[i + 1];
    }
  }
  return {};
}

std::string parseOptionValue(const std::vector<std::string>& args,
                             const std::string& option) {
  for (size_t i = 0; i + 1 < args.size(); ++i) {
    if (args[i] == option) {
      return args[i + 1];
    }
  }
  return {};
}

bool hasFlag(const std::vector<std::string>& args, const std::string& flag) {
  return std::find(args.begin(), args.end(), flag) != args.end();
}

std::vector<std::string> parsePortTargets(const std::vector<std::string>& args) {
  std::vector<std::string> ports;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--group" || args[i] == "--root") {
      ++i;
      continue;
    }
    if (!args[i].empty() && args[i][0] == '-') {
      continue;
    }
    ports.push_back(args[i]);
  }
  return ports;
}

std::string depToName(std::string_view dep) {
  const auto at = dep.find('@');
  if (at == std::string_view::npos) {
    return std::string(dep);
  }
  return std::string(dep.substr(0, at));
}

std::string joinPaths(const std::vector<std::string>& items) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (items[i].empty()) {
      continue;
    }
    if (!out.empty()) {
      out += ":";
    }
    out += items[i];
  }
  return out;
}

int runValidate(const std::filesystem::path& root) {
  auto cfg = ConfigStore::load(root);
  if (!cfg.ok()) {
    printStatusError(cfg.status());
    return 1;
  }
  auto s = ConfigStore::validate(root);
  if (!s.ok()) {
    printStatusError(s);
    return 1;
  }
  s = GroupStore::validateAll(root, cfg.value());
  if (!s.ok()) {
    printStatusError(s);
    return 1;
  }
  s = PortStore::validateAll(root, cfg.value());
  if (!s.ok()) {
    printStatusError(s);
    return 1;
  }
  std::cout << "validate: ok\n";
  return 0;
}

Result<ResolveResult> resolveFromArgs(const std::filesystem::path& root,
                                      const std::vector<std::string>& args,
                                      Config* out_cfg,
                                      Group* out_group) {
  auto cfg = ConfigStore::load(root);
  if (!cfg.ok()) {
    return cfg.status();
  }
  Group resolved_group;
  std::string group_name = parseGroup(args);
  if (!group_name.empty()) {
    auto group = GroupStore::loadByName(root, cfg.value(), group_name);
    if (!group.ok()) {
      return group.status();
    }
    resolved_group = group.value();
  } else {
    auto ports = parsePortTargets(args);
    if (ports.empty()) {
      return Status{StatusCode::kInvalidArgument,
                    "Provide --group <name> or one or more port names"};
    }
    resolved_group.name = "adhoc";
    resolved_group.summary = "ad-hoc targets";
    resolved_group.ports = std::move(ports);
  }

  auto resolved = Resolver::resolveGroup(root, cfg.value(), resolved_group);
  if (!resolved.ok()) {
    return resolved.status();
  }

  if (out_cfg) {
    *out_cfg = cfg.value();
  }
  if (out_group) {
    *out_group = resolved_group;
  }

  return resolved;
}

Result<Lockfile> lockfileForResolved(const std::filesystem::path& root,
                                     const Config& cfg,
                                     const ResolveResult& resolved,
                                     bool require_store = true) {
  Lockfile lock;
  for (const auto& name : resolved.order) {
    const auto& recipe = resolved.nodes.at(name).recipe;
    LockEntry entry;
    entry.name = recipe.name;
    entry.version = recipe.version;
    entry.status = "reused";
    entry.recipe = std::filesystem::relative(recipe.recipe_path, root).string();
    entry.store = cfg.layout.store_dir + "/" +
                  hashKey(recipe.name + "@" + recipe.version).substr(0, 12) +
                  "-" + recipe.name + "-" + recipe.version;
    entry.deps = recipe.deps;
    if (require_store) {
      const auto store_dir = root / entry.store;
      if (!std::filesystem::exists(store_dir) || std::filesystem::is_empty(store_dir)) {
        return Status{StatusCode::kNotFound,
                      "Store path missing for apply: " + recipe.name + " at " +
                          store_dir.string()};
      }
    }
    lock.entries.push_back(std::move(entry));
  }
  return lock;
}

int runResolve(const std::filesystem::path& root,
               const std::vector<std::string>& args) {
  auto resolved = resolveFromArgs(root, args, nullptr, nullptr);
  if (!resolved.ok()) {
    printStatusError(resolved.status());
    return 1;
  }

  std::cout << "resolved order:\n";
  for (const auto& name : resolved.value().order) {
    const auto& recipe = resolved.value().nodes.at(name).recipe;
    std::cout << "  - " << recipe.name << "@" << recipe.version
              << " (" << recipe.build.system << ")\n";
  }
  return 0;
}

int runBuild(const std::filesystem::path& root,
             const std::vector<std::string>& args) {
  Config cfg;
  Group group;
  auto resolved = resolveFromArgs(root, args, &cfg, &group);
  if (!resolved.ok()) {
    printStatusError(resolved.status());
    return 1;
  }

  Lockfile lock;
  lock.schema = 1;
  lock.state = "planned";
  {
    std::ostringstream cmd;
    cmd << "pkg build";
    for (size_t i = 1; i < args.size(); ++i) {
      cmd << " " << args[i];
    }
    lock.run_command = cmd.str();
  }
  lock.started_unix = static_cast<long long>(std::time(nullptr));
  for (const auto& name : resolved.value().order) {
    const auto& recipe = resolved.value().nodes.at(name).recipe;
    LockEntry entry;
    entry.name = recipe.name;
    entry.version = recipe.version;
    entry.status = "planned";
    entry.recipe = std::filesystem::relative(recipe.recipe_path, root).string();
    entry.store = cfg.layout.store_dir + "/" +
                  hashKey(recipe.name + "@" + recipe.version).substr(0, 12) +
                  "-" + recipe.name + "-" + recipe.version;
    entry.log = (std::filesystem::path(cfg.layout.build_dir) / "logs" /
                 (recipe.name + "-" + recipe.version + ".log"))
                    .string();
    entry.deps = recipe.deps;
    lock.entries.push_back(std::move(entry));
  }
  std::unordered_map<std::string, std::string> store_by_name;
  for (const auto& entry : lock.entries) {
    store_by_name[entry.name] = entry.store;
  }

  const auto logs_dir = root / cfg.layout.build_dir / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  if (ec) {
    printStatusError(Status{StatusCode::kIoError,
                            "Failed to create logs dir: " + logs_dir.string()});
    return 1;
  }

  bool has_failure = false;
  std::unordered_set<std::string> failed_or_skipped;
  const int jobs = cfg.build.jobs > 0
                       ? cfg.build.jobs
                       : std::max(1u, std::thread::hardware_concurrency());
  int built_count = 0;
  int reused_count = 0;
  int failed_count = 0;
  int skipped_count = 0;
  int planned_count = 0;
  for (size_t i = 0; i < lock.entries.size(); ++i) {
    auto& entry = lock.entries[i];
    const auto& recipe = resolved.value().nodes.at(entry.name).recipe;
    const auto recipe_dir = recipe.recipe_path.parent_path();
    const auto src_dir =
        root / cfg.layout.build_dir / "src" / (recipe.name + "-" + recipe.version);
    const auto build_dir =
        root / cfg.layout.build_dir / (recipe.name + "-" + recipe.version);
    const auto downloads_dir = root / cfg.layout.build_dir / "downloads";
    const auto store_dir = root / entry.store;
    const auto log_path =
        logs_dir / (recipe.name + "-" + recipe.version + ".log");
    entry.error.clear();
    const auto entry_start = std::time(nullptr);

    bool dependency_failed = false;
    std::string failed_dep_name;
    for (const auto& dep : entry.deps) {
      const std::string dep_name = depToName(dep);
      if (failed_or_skipped.find(dep_name) != failed_or_skipped.end()) {
        dependency_failed = true;
        failed_dep_name = dep_name;
        break;
      }
    }
    if (dependency_failed) {
      entry.status = "skipped";
      entry.error = "dependency failed or skipped: " + failed_dep_name;
      entry.build_seconds = 0;
      failed_or_skipped.insert(entry.name);
      ++skipped_count;
      continue;
    }

    if (std::filesystem::exists(store_dir) &&
        !std::filesystem::is_empty(store_dir, ec)) {
      entry.status = "reused";
      entry.build_seconds = 0;
      ++reused_count;
      continue;
    }

    std::filesystem::create_directories(src_dir, ec);
    if (ec) {
      entry.status = "failed";
      entry.error = "Failed to create source dir: " + src_dir.string();
      has_failure = true;
      failed_or_skipped.insert(entry.name);
      entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
      if (!cfg.build.keep_failed_build_dirs) {
        std::filesystem::remove_all(build_dir, ec);
      }
      ++failed_count;
      continue;
    }
    std::filesystem::remove_all(build_dir, ec);
    ec.clear();
    std::filesystem::create_directories(build_dir, ec);
    if (ec) {
      entry.status = "failed";
      entry.error = "Failed to create build dir: " + build_dir.string();
      has_failure = true;
      failed_or_skipped.insert(entry.name);
      entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
      if (!cfg.build.keep_failed_build_dirs) {
        std::filesystem::remove_all(build_dir, ec);
      }
      ++failed_count;
      continue;
    }
    std::filesystem::remove_all(store_dir, ec);
    ec.clear();
    std::filesystem::create_directories(store_dir, ec);
    if (ec) {
      entry.status = "failed";
      entry.error = "Failed to create store dir: " + store_dir.string();
      has_failure = true;
      failed_or_skipped.insert(entry.name);
      entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
      if (!cfg.build.keep_failed_build_dirs) {
        std::filesystem::remove_all(build_dir, ec);
      }
      ++failed_count;
      continue;
    }

    const auto patch_script = recipe.scripts.patch.empty()
                                  ? std::filesystem::path{}
                                  : recipe_dir / recipe.scripts.patch;
    const auto build_script = recipe_dir / recipe.scripts.build;
    const auto install_script = recipe_dir / recipe.scripts.install;
    const auto check_script = recipe.scripts.check.empty()
                                  ? std::filesystem::path{}
                                  : recipe_dir / recipe.scripts.check;
    {
      auto s = validateInstallScriptSafety(recipe, install_script);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds =
            static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        ++failed_count;
        continue;
      }
    }
    std::vector<std::string> path_parts;
    for (const auto& dep : entry.deps) {
      const std::string dep_name = depToName(dep);
      auto it = store_by_name.find(dep_name);
      if (it != store_by_name.end()) {
        path_parts.push_back((root / it->second / "bin").string());
      }
    }
    path_parts.push_back((root / entry.store / "bin").string());
    const std::string dep_path_prefix = joinPaths(path_parts);

    {
      std::ofstream trunc(log_path, std::ios::trunc);
      if (!trunc) {
        entry.status = "failed";
        entry.error = "Failed to initialize log file: " + log_path.string();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        if (!cfg.build.keep_failed_build_dirs) {
          std::filesystem::remove_all(build_dir, ec);
        }
        ++failed_count;
        continue;
      }
    }

    {
      std::cout << "==> fetch " << recipe.name << "-" << recipe.version
                << "  log=" << log_path.string() << "\n";
      auto s = prepareSource(root, recipe, src_dir, downloads_dir, log_path);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        if (!cfg.build.keep_failed_build_dirs) {
          std::filesystem::remove_all(build_dir, ec);
        }
        ++failed_count;
        continue;
      }
    }

    if (!patch_script.empty()) {
      std::cout << "==> patch " << recipe.name << "-" << recipe.version
                << "  script=" << patch_script.filename().string() << "\n";
      auto s = runScript(patch_script, log_path, recipe, root, src_dir, build_dir,
                         store_dir, dep_path_prefix, jobs);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        if (!cfg.build.keep_failed_build_dirs) {
          std::filesystem::remove_all(build_dir, ec);
        }
        ++failed_count;
        continue;
      }
    }
    {
      std::cout << "==> build " << recipe.name << "-" << recipe.version
                << "  script=" << build_script.filename().string() << "\n";
      auto s = runScript(build_script, log_path, recipe, root, src_dir, build_dir,
                         store_dir, dep_path_prefix, jobs);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        if (!cfg.build.keep_failed_build_dirs) {
          std::filesystem::remove_all(build_dir, ec);
        }
        ++failed_count;
        continue;
      }
    }
    {
      std::cout << "==> install " << recipe.name << "-" << recipe.version
                << "  script=" << install_script.filename().string() << "\n";
      auto s = runScript(install_script, log_path, recipe, root, src_dir,
                         build_dir, store_dir, dep_path_prefix, jobs);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        ++failed_count;
        continue;
      }
    }
    if (!check_script.empty()) {
      std::cout << "==> check " << recipe.name << "-" << recipe.version
                << "  script=" << check_script.filename().string() << "\n";
      auto s = runScript(check_script, log_path, recipe, root, src_dir, build_dir,
                         store_dir, dep_path_prefix, jobs);
      if (!s.ok()) {
        entry.status = "failed";
        entry.error = s.message();
        has_failure = true;
        failed_or_skipped.insert(entry.name);
        entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
        if (!cfg.build.keep_failed_build_dirs) {
          std::filesystem::remove_all(build_dir, ec);
        }
        ++failed_count;
        continue;
      }
    }

    entry.status = "built";
    entry.build_seconds = static_cast<int>(std::max<std::time_t>(0, std::time(nullptr) - entry_start));
    if (!cfg.build.keep_build_dirs) {
      std::filesystem::remove_all(build_dir, ec);
    }
    ++built_count;
  }

  for (const auto& entry : lock.entries) {
    if (entry.status == "planned") {
      ++planned_count;
    }
  }

  lock.state = has_failure ? "failed" : "done";
  lock.finished_unix = static_cast<long long>(std::time(nullptr));
  lock.summary_planned = planned_count;
  lock.summary_built = built_count;
  lock.summary_reused = reused_count;
  lock.summary_failed = failed_count;
  lock.summary_skipped = skipped_count;
  auto save = LockfileStore::save(root, cfg, lock);
  if (!save.ok()) {
    printStatusError(save);
    return 1;
  }

  std::cout << "build: processed " << lock.entries.size() << " ports into "
            << (root / cfg.layout.lockfile).string() << "\n";
  std::cout << "build: built=" << built_count
            << " reused=" << reused_count
            << " failed=" << failed_count
            << " skipped=" << skipped_count
            << " planned=" << planned_count << "\n";
  if (has_failure) {
    return 1;
  }

  if (hasFlag(args, "--activate")) {
    std::vector<std::string> apply_args;
    apply_args.emplace_back("apply");
    const auto group_name = parseGroup(args);
    if (!group_name.empty()) {
      apply_args.emplace_back("--group");
      apply_args.emplace_back(group_name);
    }
    return runApply(root, apply_args);
  }

  return 0;
}

int runPackage(const std::filesystem::path& root,
               const std::vector<std::string>& args) {
  Config cfg;
  auto resolved = resolveFromArgs(root, args, &cfg, nullptr);
  if (!resolved.ok()) {
    printStatusError(resolved.status());
    return 1;
  }

  auto lockfile = lockfileForResolved(root, cfg, resolved.value(), false);
  if (!lockfile.ok()) {
    printStatusError(lockfile.status());
    return 1;
  }

  auto out_dir_text = parseOptionValue(args, "--out");
  std::filesystem::path out_dir;
  if (!out_dir_text.empty()) {
    out_dir = out_dir_text;
    if (!out_dir.is_absolute()) {
      out_dir = root / out_dir;
    }
  } else {
    out_dir = root / cfg.layout.build_dir / "packages";
  }

  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);
  if (ec) {
    printStatusError(Status{StatusCode::kIoError,
                            "Failed to create package output dir: " +
                                out_dir.string()});
    return 1;
  }

  const auto logs_dir = root / cfg.layout.build_dir / "logs";
  std::filesystem::create_directories(logs_dir, ec);
  if (ec) {
    printStatusError(Status{StatusCode::kIoError,
                            "Failed to create logs dir: " + logs_dir.string()});
    return 1;
  }

  int packaged_count = 0;
  for (const auto& entry : lockfile.value().entries) {
    const std::filesystem::path store_dir = root / entry.store;
    const std::string store_dir_name = store_dir.filename().string();
    const std::filesystem::path artifact_path = out_dir / (store_dir_name + ".pkg");
    const std::filesystem::path log_path =
        logs_dir / (entry.name + "-" + entry.version + ".package.log");

    std::filesystem::remove(artifact_path, ec);
    ec.clear();

    auto s = runCommandToLog(
        "tar -C " + shellQuote(store_dir.parent_path().string()) + " -cf - " +
            shellQuote(store_dir_name) + " | zstd -q -19 -o " +
            shellQuote(artifact_path.string()),
        log_path,
        true);
    if (!s.ok()) {
      printStatusError(s);
      return 1;
    }

    std::cout << "package: wrote " << artifact_path.string() << "\n";
    ++packaged_count;
  }

  std::cout << "package: created " << packaged_count << " package artifacts in "
            << out_dir.string() << "\n";
  return 0;
}

int runDownload(const std::filesystem::path& root,
                const std::vector<std::string>& args) {
  Config cfg;
  auto resolved = resolveFromArgs(root, args, &cfg, nullptr);
  if (!resolved.ok()) {
    printStatusError(resolved.status());
    return 1;
  }

  auto lockfile = lockfileForResolved(root, cfg, resolved.value());
  if (!lockfile.ok()) {
    printStatusError(lockfile.status());
    return 1;
  }

  std::filesystem::path from_dir = cfg.packages.source_dir;
  if (!from_dir.is_absolute()) {
    from_dir = root / from_dir;
  }

  const bool activate = hasFlag(args, "--activate");
  const auto logs_dir = root / cfg.layout.build_dir / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logs_dir, ec);
  if (ec) {
    printStatusError(Status{StatusCode::kIoError,
                            "Failed to create logs dir: " + logs_dir.string()});
    return 1;
  }

  int extracted_count = 0;
  for (const auto& entry : lockfile.value().entries) {
    const std::filesystem::path store_dir = root / entry.store;
    if (std::filesystem::exists(store_dir) && !std::filesystem::is_empty(store_dir)) {
      std::cout << "download: reused " << store_dir.string() << "\n";
      continue;
    }

    const std::string store_dir_name = store_dir.filename().string();
    const std::filesystem::path artifact_path = from_dir / (store_dir_name + ".pkg");
    if (!std::filesystem::exists(artifact_path)) {
      printStatusError(Status{StatusCode::kNotFound,
                              "Package artifact missing: " + artifact_path.string()});
      return 1;
    }

    std::filesystem::create_directories(store_dir.parent_path(), ec);
    if (ec) {
      printStatusError(Status{StatusCode::kIoError,
                              "Failed to create store dir parent: " +
                                  store_dir.parent_path().string()});
      return 1;
    }

    const std::filesystem::path log_path =
        logs_dir / (entry.name + "-" + entry.version + ".download.log");
    auto s = runCommandToLog(
        "zstd -dc " + shellQuote(artifact_path.string()) + " | tar -C " +
            shellQuote(store_dir.parent_path().string()) + " -xf -",
        log_path,
        true);
    if (!s.ok()) {
      printStatusError(s);
      return 1;
    }

    if (!std::filesystem::exists(store_dir) || std::filesystem::is_empty(store_dir)) {
      printStatusError(Status{StatusCode::kInternalError,
                              "Extracted package missing store dir: " +
                                  store_dir.string()});
      return 1;
    }

    std::cout << "download: extracted " << artifact_path.string() << " -> "
              << store_dir.string() << "\n";
    ++extracted_count;
  }

  std::cout << "download: extracted " << extracted_count << " package artifacts from "
            << from_dir.string() << "\n";

  if (activate) {
    std::vector<std::string> apply_args;
    apply_args.emplace_back("apply");
    const auto group_name = parseGroup(args);
    if (!group_name.empty()) {
      apply_args.emplace_back("--group");
      apply_args.emplace_back(group_name);
    }
    return runApply(root, apply_args);
  }

  return 0;
}

int runApply(const std::filesystem::path& root, const std::vector<std::string>& args) {
  auto cfg = ConfigStore::load(root);
  if (!cfg.ok()) {
    printStatusError(cfg.status());
    return 1;
  }
  auto s = Status::Ok();
  const auto profile_opt = parseOptionValue(args, "--profile");
  const auto activate_target_opt = parseOptionValue(args, "--activate-target");
  const auto group_name = parseGroup(args);
  const bool force_replace = hasFlag(args, "--force");
  if (!profile_opt.empty() && !activate_target_opt.empty()) {
    printStatusError(Status{StatusCode::kInvalidArgument,
                            "Use either --profile or --activate-target, not both"});
    return 1;
  }
  if (!group_name.empty() &&
      (!profile_opt.empty() || !activate_target_opt.empty())) {
    printStatusError(Status{StatusCode::kInvalidArgument,
                            "Use --group only with the default apply target"});
    return 1;
  }

  if (profile_opt.empty() && activate_target_opt.empty()) {
    Result<Lockfile> lockfile = Status{StatusCode::kInternalError, "uninitialized"};
    std::unordered_set<std::string> allowed_names;
    const std::unordered_set<std::string>* allowed_ptr = nullptr;
    if (!group_name.empty()) {
      ResolveResult combined_resolved;
      auto append_resolved = [&](const ResolveResult& resolved) {
        for (const auto& name : resolved.order) {
          if (combined_resolved.nodes.find(name) != combined_resolved.nodes.end()) {
            continue;
          }
          combined_resolved.order.push_back(name);
          combined_resolved.nodes.emplace(name, resolved.nodes.at(name));
        }
      };

      if (group_name != "base") {
        auto base_group = GroupStore::loadByName(root, cfg.value(), "base");
        if (!base_group.ok()) {
          printStatusError(base_group.status());
          return 1;
        }
        auto base_resolved =
            Resolver::resolveGroup(root, cfg.value(), base_group.value());
        if (!base_resolved.ok()) {
          printStatusError(base_resolved.status());
          return 1;
        }
        append_resolved(base_resolved.value());
        for (const auto& name : base_resolved.value().order) {
          allowed_names.insert(name);
        }
      }

      auto group = GroupStore::loadByName(root, cfg.value(), group_name);
      if (!group.ok()) {
        printStatusError(group.status());
        return 1;
      }
      auto resolved = Resolver::resolveGroup(root, cfg.value(), group.value());
      if (!resolved.ok()) {
        printStatusError(resolved.status());
        return 1;
      }
      append_resolved(resolved.value());
      for (const auto& name : resolved.value().order) {
        allowed_names.insert(name);
      }
      allowed_ptr = &allowed_names;
      lockfile = lockfileForResolved(root, cfg.value(), combined_resolved);
    }
    if (group_name.empty()) {
      lockfile = LockfileStore::load(root, cfg.value());
    }
    if (!lockfile.ok()) {
      printStatusError(lockfile.status());
      return 1;
    }
    int linked_files = 0;
    s = materializeProfile(root, cfg.value(), lockfile.value(), allowed_ptr,
                           &linked_files);
    if (!s.ok()) {
      printStatusError(s);
      return 1;
    }
    std::cout << "apply: materialized profile " << cfg.value().layout.current_profile
              << " with " << linked_files << " linked files\n";
  }

  std::filesystem::path activation_target;
  if (!activate_target_opt.empty()) {
    activation_target = activate_target_opt;
  } else if (!profile_opt.empty()) {
    std::filesystem::path profile_path = profile_opt;
    if (!profile_path.is_absolute()) {
      profile_path = (root / cfg.value().layout.profile_dir) / profile_path;
    }
    activation_target = profile_path;
  } else {
    activation_target = cfg.value().profile.activate_target;
  }

  std::cout << "apply: target profile " << activation_target.string() << "\n";
  std::cout << "apply: activation symlink " << cfg.value().profile.activate_symlink << "\n";
  s = projectActivatedProfile(root, activation_target);
  if (!s.ok()) {
    printStatusError(s);
    return 1;
  }
  std::cout << "apply: projected active profile into target root\n";

  s = updateActivationSymlink(root, cfg.value(), activation_target, force_replace);
  if (!s.ok()) {
    std::cout << "apply: warning: " << s.message() << "\n";
  } else {
    std::cout << "apply: activation symlink updated\n";
  }
  return 0;
}

int runActivate(const std::filesystem::path& root,
                const std::vector<std::string>& args) {
  std::vector<std::string> apply_args;
  apply_args.reserve(args.size());
  apply_args.emplace_back("apply");
  for (size_t i = 1; i < args.size(); ++i) {
    apply_args.push_back(args[i]);
  }
  return runApply(root, apply_args);
}

}  // namespace

int Commands::run(int argc, char** argv) {
  if (argc < 2) {
    printUsage();
    return 1;
  }

  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(argc));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  const std::string command = args[0];
  const auto root = parseRoot(args);

  if (command == "validate") {
    return runValidate(root);
  }
  if (command == "resolve") {
    return runResolve(root, args);
  }
  if (command == "build") {
    return runBuild(root, args);
  }
  if (command == "package") {
    return runPackage(root, args);
  }
  if (command == "download") {
    return runDownload(root, args);
  }
  if (command == "activate") {
    return runActivate(root, args);
  }
  if (command == "apply") {
    return runApply(root, args);
  }
  if (command == "--help" || command == "help") {
    printUsage();
    return 0;
  }

  std::cerr << "Unknown command: " << command << "\n";
  printUsage();
  return 1;
}

}  // namespace pkg
