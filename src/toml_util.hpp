#pragma once

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/result.hpp"
#include "tomlcpp.hpp"

namespace pkg::toml_util {

inline Result<toml::Result> parseFile(const std::filesystem::path& path) {
  FILE* fp = std::fopen(path.c_str(), "rb");
  if (fp == nullptr) {
    return Status{StatusCode::kIoError,
                  "Failed to open file: " + path.string()};
  }
  toml::Result parsed = toml::parse_file(fp);
  std::fclose(fp);
  if (!parsed.ok()) {
    return Status{StatusCode::kParseError,
                  "TOML parse error in " + path.string() + ": " + parsed.errmsg()};
  }
  return parsed;
}

inline std::optional<std::string> getString(const toml::Datum& d,
                                            std::string_view key) {
  auto v = d.get(key);
  if (!v.has_value()) {
    return std::nullopt;
  }
  auto s = v->as_str();
  if (!s.has_value()) {
    return std::nullopt;
  }
  return std::string(*s);
}

inline std::optional<int> getInt(const toml::Datum& d, std::string_view key) {
  auto v = d.get(key);
  if (!v.has_value()) {
    return std::nullopt;
  }
  auto i = v->as_int();
  if (!i.has_value()) {
    return std::nullopt;
  }
  return static_cast<int>(*i);
}

inline Result<std::vector<std::string>> getStringArray(const toml::Datum& d,
                                                       std::string_view key) {
  auto v = d.get(key);
  if (!v.has_value()) {
    return std::vector<std::string>{};
  }
  auto arr = v->as_strvec();
  if (!arr.has_value()) {
    return Status{StatusCode::kParseError,
                  "Expected string array for key: " + std::string(key)};
  }
  std::vector<std::string> out;
  out.reserve(arr->size());
  for (std::string_view s : *arr) {
    out.emplace_back(s);
  }
  return out;
}

inline Status requireNonEmpty(const std::string& value,
                              std::string_view field_name,
                              const std::filesystem::path& path) {
  if (value.empty()) {
    return Status{StatusCode::kParseError,
                  "Missing or empty '" + std::string(field_name) + "' in " +
                      path.string()};
  }
  return Status::Ok();
}

}  // namespace pkg::toml_util
