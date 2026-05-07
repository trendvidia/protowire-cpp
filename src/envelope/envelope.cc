// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/envelope.h"

namespace protowire::envelope {

namespace {
const std::string kEmpty;
}

AppError& AppError::WithField(std::string field,
                              std::string err_code,
                              std::string msg,
                              std::vector<std::string> args) {
  details.push_back(
      FieldError{std::move(field), std::move(err_code), std::move(msg), std::move(args)});
  return *this;
}

AppError& AppError::WithMeta(std::string key, std::string value) {
  metadata[std::move(key)] = std::move(value);
  return *this;
}

Envelope Envelope::OK(int32_t status, std::vector<uint8_t> data) {
  Envelope e;
  e.status = status;
  e.data = std::move(data);
  return e;
}

Envelope Envelope::Err(int32_t status,
                       std::string code,
                       std::string message,
                       std::vector<std::string> args) {
  Envelope e;
  e.status = status;
  e.error = std::make_unique<AppError>();
  e.error->code = std::move(code);
  e.error->message = std::move(message);
  e.error->args = std::move(args);
  return e;
}

Envelope Envelope::TransportErr(std::string err) {
  Envelope e;
  e.transport_error = std::move(err);
  return e;
}

const std::string& Envelope::ErrorCode() const {
  if (error) return error->code;
  return kEmpty;
}

std::unordered_map<std::string, const FieldError*> Envelope::FieldErrors() const {
  std::unordered_map<std::string, const FieldError*> m;
  if (!error || error->details.empty()) return m;
  m.reserve(error->details.size());
  for (const auto& fe : error->details) m.emplace(fe.field, &fe);
  return m;
}

}  // namespace protowire::envelope
