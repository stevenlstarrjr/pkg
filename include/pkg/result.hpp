#pragma once

#include <optional>
#include <string>
#include <utility>

namespace pkg {

enum class StatusCode {
  kOk = 0,
  kInvalidArgument,
  kParseError,
  kIoError,
  kNotFound,
  kConflict,
  kInternalError,
};

class Status {
 public:
  Status() = default;
  explicit Status(StatusCode code, std::string message = {})
      : code_(code), message_(std::move(message)) {}

  static Status Ok() { return Status(StatusCode::kOk); }

  bool ok() const noexcept { return code_ == StatusCode::kOk; }
  StatusCode code() const noexcept { return code_; }
  const std::string& message() const noexcept { return message_; }

 private:
  StatusCode code_ = StatusCode::kOk;
  std::string message_;
};

template <typename T>
class Result {
 public:
  Result(const T& value) : status_(Status::Ok()), value_(value) {}
  Result(T&& value) : status_(Status::Ok()), value_(std::move(value)) {}
  Result(Status status) : status_(std::move(status)) {}

  bool ok() const noexcept { return status_.ok(); }
  const Status& status() const noexcept { return status_; }

  const T& value() const& { return *value_; }
  T& value() & { return *value_; }
  T&& value() && { return std::move(*value_); }

 private:
  Status status_;
  std::optional<T> value_;
};

}  // namespace pkg
