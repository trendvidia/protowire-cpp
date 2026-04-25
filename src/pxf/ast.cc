#include "protowire/pxf/ast.h"

namespace protowire::pxf {

Position EntryPos(const EntryPtr& e) {
  return std::visit([](auto& p) { return p->pos; }, e);
}

Position ValuePos(const ValuePtr& v) {
  return std::visit([](auto& p) { return p->pos; }, v);
}

}  // namespace protowire::pxf
