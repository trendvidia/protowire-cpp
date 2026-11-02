// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// The validation seam. Mirrors protowire-go's check package.
//
// A decoder takes an optional Validator, which runs after a successful decode
// and reports data violations. This header owns only the seam: the interface,
// the report and the error text. Validation engines live elsewhere and plug in
// here.
//
//   pxf::UnmarshalOptions opts;
//   opts.validator = &my_validator;
//   check::Report report;
//   opts.report = &report;  // for the violations, since a Status has only text
//   Status st = pxf::Unmarshal(data, msg, opts);
//   if (!st.ok()) for (const auto& v : report.violations) { ... }
//
// pb::UnmarshalOptions takes the same two fields, and decodes to a struct, so a
// validator for pb overrides ValidateStruct, or derives from StructValidator<T>.

#pragma once

#include <google/protobuf/message.h>

#include "protowire/check_seam.h"
