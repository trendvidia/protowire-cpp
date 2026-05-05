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
  // root + block_length + sum-of-prior-groups bytes. Every span/offset
  // computation here is bounded against data_.size() — HARDENING.md § SBE
  // step 3 requires that `pos + 4 + count*block_length` is checked in
  // 64-bit before being used as a span offset.
  size_t pos = block_.size();  // Root block ends at block_.size() bytes.
  for (const GroupTemplate& gt : *groups_) {
    // Use checked subtraction to keep `pos + 8 + 4` from wrapping size_t.
    if (data_.size() < 8u + 4u || pos > data_.size() - 8u - 4u) break;
    uint16_t entry_block = LoadU16(data_.data() + 8 + pos);
    uint16_t count = LoadU16(data_.data() + 8 + pos + 2);
    // Reject the same zero-block-length × non-zero-count amplification
    // pattern the active decoder rejects.
    if (entry_block == 0 && count > 0) return GroupView({}, 0, 0, nullptr);
    size_t body = size_t{entry_block} * count;
    if (data_.size() - 8u - pos - 4u < body) break;
    if (gt.fd->name() == name) {
      auto group_data = data_.subspan(8 + pos + 4, body);
      return GroupView(group_data, entry_block, count, &gt.fields);
    }
    pos += 4 + body;
  }
  return GroupView({}, 0, 0, nullptr);
}

View View::Composite(std::string_view name) const {
  const FieldTemplate* ft = FindField(name);
  if (!ft || ft->composite.empty()) {
    return View({}, {}, nullptr, nullptr, nullptr);
  }
  // HARDENING.md § SBE step 5: validate composite_offset+composite_size ≤
  // enclosing_block.size before constructing the sub-view.
  if (size_t{ft->offset} + ft->size > block_.size()) {
    return View({}, {}, nullptr, nullptr, nullptr);
  }
  return View(data_, block_.subspan(ft->offset, ft->size), tmpl_,
              &ft->composite, nullptr);
}

View GroupView::Entry(size_t i) const {
  // Bounds-check both the start and the end of the entry sub-span before
  // calling subspan (whose precondition violation is UB).
  if (block_length_ == 0 ||
      i >= data_.size() / block_length_ ||
      (i + 1) * block_length_ > data_.size()) {
    return View({}, {}, nullptr, fields_, nullptr);
  }
  auto entry = data_.subspan(i * block_length_, block_length_);
  return View({}, entry, nullptr, fields_, nullptr);
}

}  // namespace protowire::sbe
