// Zero-allocation read-only View / GroupView accessors.

#include "protowire/sbe.h"

#include <cstring>

namespace protowire::sbe {

namespace {

uint16_t LoadU16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
uint32_t LoadU32(const uint8_t* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (i * 8);
  return v;
}
uint64_t LoadU64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (i * 8);
  return v;
}

}  // namespace

const FieldTemplate* View::FindField(std::string_view name) const {
  if (!fields_) return nullptr;
  for (const FieldTemplate& ft : *fields_) {
    if (ft.fd->name() == name) return &ft;
  }
  return nullptr;
}

bool View::Bool(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft || ft->size == 0) return false;
  return block_[ft->offset] != 0;
}

int64_t View::Int(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft) return 0;
  const uint8_t* p = block_.data() + ft->offset;
  switch (ft->size) {
    case 1: return static_cast<int8_t>(*p);
    case 2: return static_cast<int16_t>(LoadU16(p));
    case 4: return static_cast<int32_t>(LoadU32(p));
    case 8: return static_cast<int64_t>(LoadU64(p));
  }
  return 0;
}

uint64_t View::Uint(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft) return 0;
  const uint8_t* p = block_.data() + ft->offset;
  switch (ft->size) {
    case 1: return *p;
    case 2: return LoadU16(p);
    case 4: return LoadU32(p);
    case 8: return LoadU64(p);
  }
  return 0;
}

double View::Float(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft) return 0.0;
  const uint8_t* p = block_.data() + ft->offset;
  if (ft->size == 4) {
    uint32_t b = LoadU32(p);
    float f;
    std::memcpy(&f, &b, 4);
    return static_cast<double>(f);
  }
  if (ft->size == 8) {
    uint64_t b = LoadU64(p);
    double d;
    std::memcpy(&d, &b, 8);
    return d;
  }
  return 0.0;
}

std::string_view View::String(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft) return {};
  const char* p = reinterpret_cast<const char*>(block_.data() + ft->offset);
  size_t n = 0;
  while (n < ft->size && p[n] != 0) ++n;
  return std::string_view(p, n);
}

std::span<const uint8_t> View::Bytes(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft) return {};
  return block_.subspan(ft->offset, ft->size);
}

GroupView View::Group(std::string_view name) const {
  if (!groups_) return GroupView({}, 0, 0, nullptr);
  // Groups follow the root block in declaration order. Each begins at
  // root + block_length + sum-of-prior-groups bytes.
  size_t pos = block_.size();  // Root block ends at block_.size() bytes.
  for (const GroupTemplate& gt : *groups_) {
    if (data_.size() < pos + 4) break;
    uint16_t entry_block = LoadU16(data_.data() + 8 + pos);
    uint16_t count = LoadU16(data_.data() + 8 + pos + 2);
    if (gt.fd->name() == name) {
      auto group_data = data_.subspan(8 + pos + 4,
                                      static_cast<size_t>(entry_block) * count);
      return GroupView(group_data, entry_block, count, &gt.fields);
    }
    pos += 4 + size_t{entry_block} * count;
  }
  return GroupView({}, 0, 0, nullptr);
}

View View::Composite(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft || ft->composite.empty()) {
    return View({}, {}, nullptr, nullptr, nullptr);
  }
  return View(data_, block_.subspan(ft->offset, ft->size), tmpl_,
              &ft->composite, nullptr);
}

View GroupView::Entry(size_t i) const {
  auto entry = data_.subspan(i * block_length_, block_length_);
  return View({}, entry, nullptr, fields_, nullptr);
}

}  // namespace protowire::sbe
