// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
#include "protowire/pxf/ast.h"

namespace protowire::pxf {

Position EntryPos(const EntryPtr& e) {
  return std::visit([](auto& p) { return p->pos; }, e);
}

Position ValuePos(const ValuePtr& v) {
  return std::visit([](auto& p) { return p->pos; }, v);
}

const char* ProtoShapeName(ProtoShape s) {
  switch (s) {
    case ProtoShape::kAnonymous:
      return "anonymous";
    case ProtoShape::kNamed:
      return "named";
    case ProtoShape::kSource:
      return "source";
    case ProtoShape::kDescriptor:
      return "descriptor";
  }
  return "?";
}

}  // namespace protowire::pxf
