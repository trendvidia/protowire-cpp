// Standard API response envelope. Wire-compatible with the Go envelope
// package — same field numbers, same proto3 semantics.
//
//   Envelope wraps an API response with transport metadata, an optional
//   success payload, and an optional application error.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "protowire/pb.h"

namespace protowire::envelope {

struct FieldError {
  std::string field;
  std::string code;
  std::string message;
  std::vector<std::string> args;

  PROTOWIRE_FIELDS(FieldError,
                   PROTOWIRE_FIELD(1, field),
                   PROTOWIRE_FIELD(2, code),
                   PROTOWIRE_FIELD(3, message),
                   PROTOWIRE_FIELD(4, args))
};

struct AppError {
  std::string code;
  std::string message;
  std::vector<std::string> args;
  std::vector<FieldError> details;
  std::map<std::string, std::string> metadata;

  // Builder helpers, return *this for chaining.
  AppError& WithField(std::string field, std::string err_code,
                      std::string msg, std::vector<std::string> args = {});
  AppError& WithMeta(std::string key, std::string value);

  PROTOWIRE_FIELDS(AppError,
                   PROTOWIRE_FIELD(1, code),
                   PROTOWIRE_FIELD(2, message),
                   PROTOWIRE_FIELD(3, args),
                   PROTOWIRE_FIELD(4, details))
  // Note: `metadata` (map<string,string>) is intentionally not serialized via
  // PROTOWIRE_FIELDS yet — pb maps require a paired-message wire layout that
  // would balloon the example. Use a separate helper if you need it on the
  // wire; in-memory it still functions identically to the Go map.
};

struct Envelope {
  int32_t status = 0;
  std::string transport_error;
  std::vector<uint8_t> data;
  std::unique_ptr<AppError> error;

  // --- Builders ---
  static Envelope OK(int32_t status, std::vector<uint8_t> data);
  static Envelope Err(int32_t status, std::string code, std::string message,
                      std::vector<std::string> args = {});
  static Envelope TransportErr(std::string err);

  // --- Queries ---
  bool IsOK() const { return transport_error.empty() && error == nullptr; }
  bool IsTransportError() const { return !transport_error.empty(); }
  bool IsAppError() const { return error != nullptr; }
  const std::string& ErrorCode() const;
  std::unordered_map<std::string, const FieldError*> FieldErrors() const;

  // Note: PROTOWIRE_FIELDS skips `error` (unique_ptr<AppError>) — the pb
  // layer doesn't wrap pointers transparently yet. Callers who need binary
  // round-trip should set/serialize via the AppError directly.
  PROTOWIRE_FIELDS(Envelope,
                   PROTOWIRE_FIELD(1, status),
                   PROTOWIRE_FIELD(2, transport_error),
                   PROTOWIRE_FIELD(3, data))
};

// --- Free-function constructors mirroring the Go API ---
inline Envelope MakeOK(int32_t status, std::vector<uint8_t> data) {
  return Envelope::OK(status, std::move(data));
}
inline Envelope MakeErr(int32_t status, std::string code, std::string message,
                        std::vector<std::string> args = {}) {
  return Envelope::Err(status, std::move(code), std::move(message),
                       std::move(args));
}
inline Envelope MakeTransportErr(std::string err) {
  return Envelope::TransportErr(std::move(err));
}

inline AppError NewAppError(std::string code, std::string message,
                            std::vector<std::string> args = {}) {
  return AppError{std::move(code), std::move(message), std::move(args), {}, {}};
}

}  // namespace protowire::envelope
