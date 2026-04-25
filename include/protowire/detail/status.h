#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace protowire {

// Lightweight Status type modelled on absl::Status. ok() means success.
// Non-ok carries a message; an optional structured Position pinpoints the
// source location for parse-time errors.
class Status {
 public:
  Status() = default;  // ok
  static Status OK() { return Status(); }
  static Status Error(std::string message) {
    return Status(std::move(message));
  }
  static Status Error(int line, int column, std::string message) {
    return Status(line, column, std::move(message));
  }

  bool ok() const { return message_.empty(); }
  explicit operator bool() const { return ok(); }

  const std::string& message() const { return message_; }
  std::optional<std::pair<int, int>> position() const {
    if (line_ <= 0) return std::nullopt;
    return std::make_pair(line_, column_);
  }

  std::string ToString() const;

 private:
  explicit Status(std::string message) : message_(std::move(message)) {}
  Status(int line, int col, std::string message)
      : message_(std::move(message)), line_(line), column_(col) {}

  std::string message_;
  int line_ = 0;
  int column_ = 0;
};

// StatusOr<T> — value or error. Mirrors absl::StatusOr<T>.
template <class T>
class StatusOr {
 public:
  StatusOr(T value) : value_(std::move(value)), status_() {}
  StatusOr(Status s) : status_(std::move(s)) {}

  bool ok() const { return status_.ok(); }
  explicit operator bool() const { return ok(); }
  const Status& status() const { return status_; }

  T& operator*() { return *value_; }
  const T& operator*() const { return *value_; }
  T* operator->() { return &*value_; }
  const T* operator->() const { return &*value_; }

  T value_or(T fallback) const {
    return ok() ? *value_ : std::move(fallback);
  }

  T&& consume() && { return std::move(*value_); }

 private:
  std::optional<T> value_;
  Status status_;
};

}  // namespace protowire
