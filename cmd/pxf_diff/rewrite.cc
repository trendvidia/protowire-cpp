// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// pxf_diff --rewrite <dir> runs every *.ops script in dir through pxf::Rewriter.
// tools/godiff/rewrite.go is the same program for protowire-go and documents
// the script format. The two have to stay line for line alike.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protowire/detail/go_quote.h"
#include "protowire/pxf.h"
#include "protowire/pxf/rewriter.h"

namespace {

namespace fs = std::filesystem;
namespace pxf = protowire::pxf;
using protowire::Status;
using protowire::detail::GoQuote;
using pxf::Assignment;
using pxf::Block;
using pxf::BlockVal;
using pxf::EntryPtr;
using pxf::ListVal;
using pxf::MapEntry;
using pxf::Rewriter;
using pxf::StringVal;
using pxf::ValuePtr;
using Entries = std::vector<EntryPtr>;

[[noreturn]] void Fatal(const std::string& msg) {
  std::cerr << msg << "\n";
  std::exit(2);
}

std::string TrimLeft(std::string_view s) {
  size_t i = s.find_first_not_of(" \t");
  return i == std::string_view::npos ? "" : std::string(s.substr(i));
}

// Token takes the first token off *s and returns it.
std::string Token(std::string* s) {
  std::string in = TrimLeft(*s);
  if (in.empty()) Fatal("missing argument");
  if (in[0] != '"') {
    size_t i = in.find_first_of(" \t");
    if (i == std::string::npos) {
      s->clear();
      return in;
    }
    *s = TrimLeft(std::string_view(in).substr(i));
    return in.substr(0, i);
  }
  std::string out;
  for (size_t i = 1; i < in.size(); ++i) {
    if (in[i] == '"') {
      *s = TrimLeft(std::string_view(in).substr(i + 1));
      return out;
    }
    if (in[i] != '\\') {
      out.push_back(in[i]);
      continue;
    }
    if (++i >= in.size()) Fatal("bad quoted token: " + in);
    switch (in[i]) {
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case '\\':
      case '"':
        out.push_back(in[i]);
        break;
      case 'x': {
        if (i + 2 >= in.size()) Fatal("bad quoted token: " + in);
        out.push_back(static_cast<char>(std::stoi(in.substr(i + 1, 2), nullptr, 16)));
        i += 2;
        break;
      }
      default:
        Fatal("bad quoted token: " + in);
    }
  }
  Fatal("bad quoted token: " + in);
}

// Cut splits at the first space, as strings.Cut.
std::pair<std::string, std::string> Cut(const std::string& s) {
  size_t i = s.find(' ');
  if (i == std::string::npos) return {s, ""};
  return {s.substr(0, i), s.substr(i + 1)};
}

pxf::Document ParseText(const std::string& text) {
  auto doc = pxf::Parse(text);
  if (!doc.ok()) Fatal("script text " + GoQuote(text) + ": " + doc.status().ToString());
  return std::move(doc).consume();
}

// ParseEntries parses inside a block, where a map entry is allowed.
Entries ParseEntries(const std::string& text) {
  pxf::Document doc = ParseText("w {\n" + text + "\n}\n");
  return std::move(std::get<std::unique_ptr<Block>>(doc.entries[0])->entries);
}

ValuePtr MakeInt(std::string raw) {
  auto v = std::make_unique<pxf::IntVal>();
  v->raw = std::move(raw);
  return v;
}
ValuePtr MakeStr(std::string s) {
  auto v = std::make_unique<StringVal>();
  v->value = std::move(s);
  return v;
}
ValuePtr MakeBad() {
  return std::make_unique<pxf::BadVal>();
}
EntryPtr MakeAssign(std::string key, ValuePtr v) {
  auto a = std::make_unique<Assignment>();
  a->key = std::move(key);
  a->value = std::move(v);
  return a;
}
EntryPtr MakeBlock(std::string name, Entries entries) {
  auto b = std::make_unique<Block>();
  b->name = std::move(name);
  b->entries = std::move(entries);
  return b;
}
template <class... A>
Entries MakeEntries(A... e) {
  Entries out;
  (out.push_back(std::move(e)), ...);
  return out;
}
Entries Body() {
  return MakeEntries(MakeAssign("a", MakeInt("1")),
                     MakeBlock("n", MakeEntries(MakeAssign("b", MakeStr("s")))));
}
ValuePtr MakeList() {
  auto l = std::make_unique<ListVal>();
  l->elements.push_back(MakeInt("1"));
  l->elements.push_back(MakeStr("s"));
  return l;
}
ValuePtr MakeBlockVal(Entries entries) {
  auto b = std::make_unique<BlockVal>();
  b->entries = std::move(entries);
  return b;
}

template <class T, class V>
const T* As(const V& v) {
  auto* p = std::get_if<std::unique_ptr<T>>(&v);
  return p ? p->get() : nullptr;
}

std::string ReadFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) Fatal("cannot read " + path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

// A value argument: built here, or a node of the Document held by address.
struct Arg {
  ValuePtr owned;
  const ValuePtr* node = nullptr;
  const ValuePtr& get() const { return node != nullptr ? *node : owned; }
};

struct NodeRef {
  const Block* block = nullptr;
  Arg val;
};

class State {
 public:
  explicit State(std::string dir) : dir_(std::move(dir)) {}

  void Reset() { r_.reset(); }

  void Run(const std::string& line);

 private:
  bool Result(const std::string& name, const Status& st) {
    ++seq_;
    if (!st.ok()) {
      std::cout << "op " << seq_ << " " << name << ": err " << GoQuote(st.ToString()) << "\n";
      return false;
    }
    std::cout << "op " << seq_ << " " << name << ": ok\n";
    return true;
  }

  void PrintBytes(const std::string& name, const Rewriter& r) {
    auto out = r.Bytes();
    if (!Result(name, out.status())) return;
    size_t start = 0;
    while (start < out->size()) {
      size_t nl = out->find('\n', start);
      size_t end = nl == std::string::npos ? out->size() : nl + 1;
      std::cout << "  | " << GoQuote(std::string_view(*out).substr(start, end - start)) << "\n";
      start = end;
    }
  }

  void Start(const std::string& name, const std::string& src) {
    r_.reset();
    auto r = Rewriter::New(src);
    if (Result(name, r.status())) r_ = std::move(r).consume();
  }

  Arg Value(const std::string& rest);
  Entries EntriesArg(const std::string& rest);
  NodeRef Node(const std::string& sel);

  using Edit = std::function<Status(Rewriter&)>;
  void Fresh(const std::string& label, const std::string& src, const Edit& edit);
  void Sweep(const std::string& name, const std::string& src);
  void Walk(const std::string& src,
            const std::string& prefix,
            const Entries& entries,
            const std::vector<int>& at);
  void Anon(const std::string& src, const std::string& path, const ListVal& list);
  void Keyed(const std::string& src,
             const std::string& path,
             const Entries& entries,
             const std::string& field);
  void KeyedOps(const std::string& src,
                const std::string& path,
                const std::vector<std::string>& keys,
                const std::vector<const Entries*>& bodies,
                const std::string& field);

  std::string dir_;
  int seq_ = 0;
  std::unique_ptr<Rewriter> r_;
};

Arg State::Value(const std::string& rest) {
  auto [head, tail] = Cut(rest);
  Arg a;
  if (head == "!nil") return a;
  if (head == "!bad") {
    a.owned = MakeBad();
  } else if (head == "!list-bad") {
    auto l = std::make_unique<ListVal>();
    l->elements.push_back(MakeStr("a"));
    l->elements.push_back(MakeBad());
    a.owned = std::move(l);
  } else if (head == "!list-nil") {
    auto l = std::make_unique<ListVal>();
    l->elements.emplace_back();
    a.owned = std::move(l);
  } else if (head == "!block-bad") {
    a.owned = MakeBlockVal(MakeEntries(MakeAssign("k", MakeBad())));
  } else if (head == "!block-nil") {
    a.owned = MakeBlockVal(MakeEntries(MakeAssign("k", ValuePtr{})));
  } else if (head == "!block-nilentry") {
    a.owned = MakeBlockVal(MakeEntries(EntryPtr{}));
  } else if (head == "!block-nested-bad") {
    a.owned = MakeBlockVal(MakeEntries(MakeBlock("n", MakeEntries(MakeAssign("a", MakeBad())))));
  } else if (head == "!raw") {
    std::string t = tail;
    a.owned = MakeInt(Token(&t));
  } else if (head == "!node") {
    return std::move(Node(tail).val);
  } else {
    pxf::Document doc = ParseText("v = " + rest);
    a.owned = std::move(std::get<std::unique_ptr<Assignment>>(doc.entries[0])->value);
  }
  return a;
}

Entries State::EntriesArg(const std::string& rest) {
  auto [head, tail] = Cut(rest);
  std::string t = tail;
  if (head == "!none") return {};
  if (head == "!nil") return MakeEntries(EntryPtr{});
  if (head == "!bad") return MakeEntries(MakeAssign("a", MakeBad()));
  if (head == "!nil-value") return MakeEntries(MakeAssign("a", ValuePtr{}));
  if (head == "!map-bad") {
    auto m = std::make_unique<MapEntry>();
    m->key = "k";
    m->value = MakeBad();
    return MakeEntries(EntryPtr(std::move(m)));
  }
  if (head == "!block-bad") {
    return MakeEntries(MakeBlock("n", MakeEntries(MakeAssign("a", MakeBad()))));
  }
  if (head == "!block-nilchild") return MakeEntries(MakeBlock("n", MakeEntries(EntryPtr{})));
  if (head == "!map" || head == "!mapq") {
    auto m = std::make_unique<MapEntry>();
    m->key = Token(&t);
    m->key_quoted = head == "!mapq";
    m->value = Value(t).owned;
    return MakeEntries(EntryPtr(std::move(m)));
  }
  if (head == "!assignq") {
    auto a = std::make_unique<Assignment>();
    a->key = Token(&t);
    a->key_quoted = true;
    a->value = Value(t).owned;
    return MakeEntries(EntryPtr(std::move(a)));
  }
  if (head == "!blockq") {
    auto b = std::make_unique<Block>();
    b->name = Token(&t);
    b->name_quoted = true;
    b->entries = EntriesArg(t);
    return MakeEntries(EntryPtr(std::move(b)));
  }
  if (head == "!type") {
    auto e = std::make_unique<pxf::TypeEntry>();
    e->url = Token(&t);
    return MakeEntries(EntryPtr(std::move(e)));
  }
  return ParseEntries(head == "!q" ? Token(&t) : rest);
}

NodeRef State::Node(const std::string& sel) {
  static Block handmade_block = [] {
    Block b;
    b.name = "x";
    return b;
  }();
  NodeRef out;
  if (sel == "!nil") return out;
  if (sel == "!handmade") {
    out.block = &handmade_block;
    out.val.owned = MakeInt("9");
    return out;
  }
  if (sel == "!bad") {
    out.val.owned = MakeBad();
    return out;
  }
  if (!r_) Fatal("node " + sel + " with no document");
  const Entries* entries = &r_->Document()->entries;
  std::stringstream steps(sel);
  std::string step;
  while (std::getline(steps, step, '.')) {
    if (step[0] == 'e') {
      const ValuePtr* cur = out.val.node;
      const ListVal* list = cur != nullptr ? As<ListVal>(*cur) : nullptr;
      size_t n = std::stoul(step.substr(1));
      if (list == nullptr || n >= list->elements.size()) Fatal("bad node " + sel);
      out.val.node = &list->elements[n];
    } else {
      size_t n = std::stoul(step);
      if (n >= entries->size()) Fatal("bad node " + sel);
      const EntryPtr& e = (*entries)[n];
      out.block = nullptr;
      out.val.node = nullptr;
      if (const auto* b = As<Block>(e)) {
        out.block = b;
        entries = &b->entries;
      } else if (const auto* a = As<Assignment>(e)) {
        out.val.node = &a->value;
      } else if (const auto* m = As<MapEntry>(e)) {
        out.val.node = &m->value;
      } else {
        Fatal("bad node " + sel);
      }
    }
    if (out.val.node != nullptr) {
      if (const auto* bv = As<BlockVal>(*out.val.node)) entries = &bv->entries;
    }
  }
  return out;
}

std::vector<std::string> Glob(const std::string& dir, const std::string& pattern) {
  // Only `<subdir>/*<ext>` is needed.
  size_t star = pattern.rfind('*');
  if (star == std::string::npos || star == 0 || pattern[star - 1] != '/') {
    Fatal("unsupported pattern " + pattern);
  }
  std::string sub = pattern.substr(0, star);
  std::string ext = pattern.substr(star + 1);
  std::vector<std::string> out;
  for (const auto& e : fs::directory_iterator(fs::path(dir) / sub)) {
    std::string name = e.path().filename().string();
    if (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
      out.push_back((fs::path(dir) / sub / name).string());
    }
  }
  std::sort(out.begin(), out.end());
  if (out.empty()) Fatal("no files match " + pattern);
  return out;
}

void State::Run(const std::string& line) {
  std::string rest = line;
  std::string name = Token(&rest);
  if (name == "doc" || name == "src" || name == "sweep" || name == "sweep-glob") {
    std::string arg = Token(&rest);
    if (name == "doc") {
      Start(name, ReadFile((fs::path(dir_) / arg).string()));
    } else if (name == "src") {
      Start(name, arg);
    } else if (name == "sweep") {
      Sweep(arg, ReadFile((fs::path(dir_) / arg).string()));
    } else {
      for (const auto& f : Glob(dir_, arg)) {
        Sweep(fs::path(f).filename().string(), ReadFile(f));
      }
    }
    return;
  }
  if (!r_) return;  // the document was refused; nothing can run until the next one
  Rewriter& r = *r_;
  Status st;
  if (name == "set") {
    std::string path = Token(&rest);
    st = r.Set(path, Value(rest).get());
  } else if (name == "remove") {
    st = r.Remove(Token(&rest));
  } else if (name == "replace") {
    std::string sel = Token(&rest);
    NodeRef old = Node(sel);
    st = r.ReplaceValue(old.val.get(), Value(rest).get());
  } else if (name == "span") {
    int start = std::stoi(Token(&rest));
    int end = std::stoi(Token(&rest));
    std::string text;
    if (rest != "!nil") text = Token(&rest);
    st = r.SetSpan(start, end, text);
  } else if (name == "append") {
    std::string sel = Token(&rest);
    NodeRef block = Node(sel);
    Entries es = EntriesArg(rest);
    if (es.size() != 1) Fatal("append takes one entry: " + line);
    st = r.AppendEntry(block.block, es[0]);
  } else if (name == "keyedby") {
    std::string path = Token(&rest);
    r.KeyedByField(path, Token(&rest));
  } else if (name == "setkeyed") {
    std::string path = Token(&rest);
    std::string key = Token(&rest);
    std::string sub = Token(&rest);
    st = r.SetKeyed(path, key, sub, Value(rest).get());
  } else if (name == "rmkeyed") {
    std::string path = Token(&rest);
    std::string key = Token(&rest);
    st = r.RemoveKeyed(path, key, Token(&rest));
  } else if (name == "rmelem") {
    std::string path = Token(&rest);
    st = r.RemoveKeyedElement(path, Token(&rest));
  } else if (name == "insert") {
    std::string path = Token(&rest);
    std::string key = Token(&rest);
    std::string before = Token(&rest);
    st = r.InsertKeyedElement(path, key, before, EntriesArg(rest));
  } else if (name == "rename") {
    std::string path = Token(&rest);
    std::string from = Token(&rest);
    st = r.RenameKeyedElement(path, from, Token(&rest));
  } else if (name == "move") {
    std::string path = Token(&rest);
    std::string key = Token(&rest);
    st = r.MoveKeyedElement(path, key, Token(&rest));
  } else if (name == "bytes") {
    PrintBytes(name, r);
    return;
  } else {
    Fatal("unknown operation: " + line);
  }
  Result(name, st);
}

// --- sweep -----------------------------------------------------------------

void State::Fresh(const std::string& label, const std::string& src, const Edit& edit) {
  auto r = Rewriter::New(src);
  if (!r.ok()) Fatal("sweep: " + r.status().ToString());
  if (Result(label, edit(**r))) PrintBytes(label + " bytes", **r);
}

void State::Sweep(const std::string& name, const std::string& src) {
  std::cout << "=== sweep " << name << "\n";
  auto r = Rewriter::New(src);
  if (!Result("new", r.status())) return;
  Walk(src, "", (*r)->Document()->entries, {});
}

const EntryPtr& EntryAt(const Rewriter& r, const std::vector<int>& at) {
  const Entries* entries = &r.Document()->entries;
  const EntryPtr* e = nullptr;
  for (int i : at) {
    e = &(*entries)[i];
    const BlockVal* bv = nullptr;
    if (const auto* n = As<Block>(*e)) {
      entries = &n->entries;
    } else if (const auto* n = As<Assignment>(*e)) {
      bv = As<BlockVal>(n->value);
    } else if (const auto* n = As<MapEntry>(*e)) {
      bv = As<BlockVal>(n->value);
    }
    if (bv != nullptr) entries = &bv->entries;
  }
  return *e;
}

void State::Walk(const std::string& src,
                 const std::string& prefix,
                 const Entries& entries,
                 const std::vector<int>& at) {
  for (size_t i = 0; i < entries.size(); ++i) {
    const EntryPtr& e = entries[i];
    std::string key;
    const ValuePtr* val = nullptr;
    const Block* block = As<Block>(e);
    if (block != nullptr) {
      key = block->name;
    } else if (const auto* n = As<Assignment>(e)) {
      key = n->key;
      val = &n->value;
    } else if (const auto* n = As<MapEntry>(e)) {
      key = n->key;
      val = &n->value;
    } else {
      continue;
    }
    if (key.find('.') != std::string::npos) continue;
    const std::string path = prefix + key;
    std::vector<int> here = at;
    here.push_back(static_cast<int>(i));
    const std::string q = GoQuote(path);
    Fresh("Remove " + q, src, [&](Rewriter& r) { return r.Remove(path); });
    Fresh("Set int " + q, src, [&](Rewriter& r) { return r.Set(path, MakeInt("7")); });
    Fresh("Set list " + q, src, [&](Rewriter& r) { return r.Set(path, MakeList()); });
    Fresh("Set block " + q, src, [&](Rewriter& r) { return r.Set(path, MakeBlockVal(Body())); });
    Fresh("Set child " + q, src, [&](Rewriter& r) { return r.Set(path + ".zz", MakeInt("1")); });
    Fresh("Set chain " + q, src, [&](Rewriter& r) { return r.Set(path + ".y.z", MakeList()); });
    if (block != nullptr) {
      Fresh("Append assign " + q, src, [&](Rewriter& r) {
        return r.AppendEntry(As<Block>(EntryAt(r, here)), MakeAssign("x", MakeInt("1")));
      });
      Fresh("Append block " + q, src, [&](Rewriter& r) {
        return r.AppendEntry(As<Block>(EntryAt(r, here)), MakeBlock("n", Body()));
      });
      Fresh("Append map " + q, src, [&](Rewriter& r) {
        auto m = std::make_unique<MapEntry>();
        m->key = "k k";
        m->value = MakeList();
        return r.AppendEntry(As<Block>(EntryAt(r, here)), EntryPtr(std::move(m)));
      });
      Keyed(src, path, block->entries, "");
      Walk(src, path + ".", block->entries, here);
      continue;
    }
    Fresh("Replace " + q, src, [&](Rewriter& r) {
      const EntryPtr& node = EntryAt(r, here);
      if (const auto* n = As<Assignment>(node)) return r.ReplaceValue(n->value, MakeList());
      return r.ReplaceValue(As<MapEntry>(node)->value, MakeList());
    });
    if (const auto* bv = As<BlockVal>(*val)) {
      Keyed(src, path, bv->entries, "");
      Walk(src, path + ".", bv->entries, here);
    } else if (const auto* list = As<ListVal>(*val)) {
      Anon(src, path, *list);
    }
  }
}

void State::Anon(const std::string& src, const std::string& path, const ListVal& list) {
  if (list.elements.empty()) return;
  const auto* first = As<BlockVal>(list.elements[0]);
  if (first == nullptr) return;
  std::string field;
  for (const auto& e : first->entries) {
    const auto* a = As<Assignment>(e);
    if (a != nullptr && As<StringVal>(a->value) != nullptr) {
      field = a->key;
      break;
    }
  }
  if (field.empty()) return;
  std::vector<std::string> keys;
  std::vector<const Entries*> bodies;
  for (const auto& v : list.elements) {
    const auto* bv = As<BlockVal>(v);
    if (bv == nullptr) continue;
    for (const auto& e : bv->entries) {
      const auto* a = As<Assignment>(e);
      if (a == nullptr || a->key != field) continue;
      if (const auto* s = As<StringVal>(a->value)) {
        keys.push_back(s->value);
        bodies.push_back(&bv->entries);
        break;
      }
    }
  }
  KeyedOps(src, path, keys, bodies, field);
}

void State::Keyed(const std::string& src,
                  const std::string& path,
                  const Entries& entries,
                  const std::string& field) {
  std::vector<std::string> keys;
  std::vector<const Entries*> bodies;
  for (const auto& e : entries) {
    if (const auto* n = As<Block>(e)) {
      keys.push_back(n->name);
      bodies.push_back(&n->entries);
    } else if (const auto* n = As<Assignment>(e)) {
      if (const auto* bv = As<BlockVal>(n->value)) {
        keys.push_back(n->key);
        bodies.push_back(&bv->entries);
      }
    }
  }
  KeyedOps(src, path, keys, bodies, field);
}

void State::KeyedOps(const std::string& src,
                     const std::string& path,
                     const std::vector<std::string>& keys,
                     const std::vector<const Entries*>& bodies,
                     const std::string& field) {
  auto run = [&](const std::string& label, const Edit& edit) {
    Fresh(label + " " + GoQuote(path), src, [&](Rewriter& r) {
      if (!field.empty()) r.KeyedByField(path, field);
      return edit(r);
    });
  };
  run("InsertKeyed end",
      [&](Rewriter& r) { return r.InsertKeyedElement(path, "ins-1", "", Body()); });
  run("SetKeyed new", [&](Rewriter& r) { return r.SetKeyed(path, "fresh", "a.b", MakeInt("1")); });
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string& k = keys[i];
    const std::string q = GoQuote(k);
    run("SetKeyed int " + q, [&](Rewriter& r) { return r.SetKeyed(path, k, "x", MakeInt("1")); });
    run("SetKeyed chain " + q, [&](Rewriter& r) { return r.SetKeyed(path, k, "n.m", MakeList()); });
    for (const auto& e : *bodies[i]) {
      std::string sub;
      if (const auto* n = As<Block>(e)) {
        sub = n->name;
      } else if (const auto* n = As<Assignment>(e)) {
        sub = n->key;
      } else if (const auto* n = As<MapEntry>(e)) {
        sub = n->key;
      }
      if (sub.empty() || sub.find('.') != std::string::npos) continue;
      run("RemoveKeyed " + q + " " + GoQuote(sub),
          [&](Rewriter& r) { return r.RemoveKeyed(path, k, sub); });
      run("SetKeyed " + q + " " + GoQuote(sub),
          [&](Rewriter& r) { return r.SetKeyed(path, k, sub, MakeInt("2")); });
    }
    run("RemoveElem " + q, [&](Rewriter& r) { return r.RemoveKeyedElement(path, k); });
    run("Rename quoted " + q, [&](Rewriter& r) { return r.RenameKeyedElement(path, k, "ren-1"); });
    run("Rename bare " + q, [&](Rewriter& r) { return r.RenameKeyedElement(path, k, "ren1"); });
    run("Move end " + q, [&](Rewriter& r) { return r.MoveKeyedElement(path, k, ""); });
    run("Insert before " + q,
        [&](Rewriter& r) { return r.InsertKeyedElement(path, "ins-1", k, Body()); });
    for (const auto& o : keys) {
      run("Move " + q + " before " + GoQuote(o),
          [&](Rewriter& r) { return r.MoveKeyedElement(path, k, o); });
    }
  }
}

}  // namespace

int RunRewrite(const char* dir) {
  std::vector<std::string> files;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ".ops") files.push_back(e.path().string());
  }
  if (files.empty()) Fatal(std::string("no scripts in ") + dir);
  std::sort(files.begin(), files.end());
  State state(dir);
  for (const auto& f : files) {
    state.Reset();
    std::stringstream lines(ReadFile(f));
    std::string line;
    int no = 0;
    while (std::getline(lines, line)) {
      ++no;
      size_t first = line.find_first_not_of(" \t\r");
      if (first == std::string::npos || line[first] == '#') continue;
      std::cout << "=== " << fs::path(f).filename().string() << ":" << no << " " << line << "\n";
      state.Run(line);
    }
  }
  return 0;
}
