#pragma once

#include <string_view>

#include "protowire/detail/status.h"
#include "protowire/pxf/ast.h"

namespace protowire::pxf {

// Parses PXF source into an AST Document with comments attached.
StatusOr<Document> Parse(std::string_view input);

}  // namespace protowire::pxf
