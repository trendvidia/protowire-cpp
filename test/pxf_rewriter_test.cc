// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
// Mirrors protowire-go's rewrite_test.go, rewrite_keyed_test.go,
// rewrite_keyed_forms_test.go and node_edit_test.go, in that order, one TEST per
// Go test. scripts/go_differential.sh runs the same edits through both ports
// and compares every error and every output byte.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "protowire/pxf.h"
#include "protowire/pxf/format.h"
#include "protowire/pxf/rewriter.h"

namespace pxf = protowire::pxf;
using protowire::Status;
using protowire::StatusOr;
using pxf::Assignment;
using pxf::BadVal;
using pxf::Block;
using pxf::BlockVal;
using pxf::BoolVal;
using pxf::EntryPtr;
using pxf::FloatVal;
using pxf::IntVal;
using pxf::ListVal;
using pxf::MapEntry;
using pxf::Rewriter;
using pxf::StringVal;
using pxf::ValuePtr;

namespace {

ValuePtr Str(std::string s) {
  auto v = std::make_unique<StringVal>();
  v->value = std::move(s);
  return v;
}
ValuePtr Int(std::string raw) {
  auto v = std::make_unique<IntVal>();
  v->raw = std::move(raw);
  return v;
}
ValuePtr Float(std::string raw) {
  auto v = std::make_unique<FloatVal>();
  v->raw = std::move(raw);
  return v;
}
ValuePtr Bool(bool b) {
  auto v = std::make_unique<BoolVal>();
  v->value = b;
  return v;
}
ValuePtr Bad() {
  return std::make_unique<BadVal>();
}
template <class... A>
ValuePtr List(A... elems) {
  auto v = std::make_unique<ListVal>();
  (v->elements.push_back(std::move(elems)), ...);
  return v;
}
template <class... A>
std::vector<EntryPtr> Body(A... entries) {
  std::vector<EntryPtr> out;
  (out.push_back(std::move(entries)), ...);
  return out;
}
EntryPtr Assign(std::string key, ValuePtr v) {
  auto a = std::make_unique<Assignment>();
  a->key = std::move(key);
  a->value = std::move(v);
  return a;
}
EntryPtr MapE(std::string key, ValuePtr v, bool quoted = false) {
  auto m = std::make_unique<MapEntry>();
  m->key = std::move(key);
  m->key_quoted = quoted;
  m->value = std::move(v);
  return m;
}
template <class... A>
EntryPtr Blk(std::string name, A... entries) {
  auto b = std::make_unique<Block>();
  b->name = std::move(name);
  (b->entries.push_back(std::move(entries)), ...);
  return b;
}

std::unique_ptr<Rewriter> MustNew(const std::string& src) {
  auto r = Rewriter::New(src);
  EXPECT_TRUE(r.ok()) << r.status().ToString();
  return std::move(r).consume();
}

std::string MustBytes(const Rewriter& r) {
  auto out = r.Bytes();
  EXPECT_TRUE(out.ok()) << out.status().ToString();
  return out.ok() ? *out : "";
}

std::string Rewrite(const std::string& src, const std::function<Status(Rewriter&)>& stage) {
  auto r = MustNew(src);
  if (r == nullptr) return "";
  Status st = stage(*r);
  EXPECT_TRUE(st.ok()) << "stage: " << st.ToString();
  return MustBytes(*r);
}

bool Has(const std::string& s, std::string_view sub) {
  return s.find(sub) != std::string::npos;
}

std::string ReplaceFirst(std::string s, const std::string& from, const std::string& to) {
  s.replace(s.find(from), from.size(), to);
  return s;
}

template <class T>
const T* Get(const EntryPtr& e) {
  return std::get<std::unique_ptr<T>>(e).get();
}

// --- rewrite_test.go -------------------------------------------------------

TEST(Rewriter, SetScalarPreservesEverythingElse) {
  std::string src = R"(# heading comment

server {
	host = "prod"   # primary
	port = 8080     # keep me
}
)";
  std::string got = Rewrite(src, [](Rewriter& r) { return r.Set("server.port", Int("9090")); });
  std::string want = R"(# heading comment

server {
	host = "prod"   # primary
	port = 9090     # keep me
}
)";
  EXPECT_EQ(got, want);
}

TEST(Rewriter, SetPreservesSeparatorAndQuoting) {
  std::string got = Rewrite("labels {\n  \"team\":   \"core\"\n}\n",
                            [](Rewriter& r) { return r.Set("labels.team", Str("infra")); });
  EXPECT_EQ(got, "labels {\n  \"team\":   \"infra\"\n}\n");
}

TEST(Rewriter, SetInsertsIntoMultilineBlock) {
  std::string got = Rewrite("server {\n\thost = \"h\"\n}\n",
                            [](Rewriter& r) { return r.Set("server.port", Int("8080")); });
  EXPECT_EQ(got, "server {\n\thost = \"h\"\n\tport = 8080\n}\n");
}

TEST(Rewriter, SetInsertsIntoInlineBlock) {
  std::string got =
      Rewrite("inline { a = 1 }\n", [](Rewriter& r) { return r.Set("inline.b", Int("2")); });
  EXPECT_EQ(got, "inline { a = 1 b = 2 }\n");
}

TEST(Rewriter, SetInsertsIntoEmptyInlineBlock) {
  std::string got = Rewrite("empty {}\n", [](Rewriter& r) { return r.Set("empty.a", Bool(true)); });
  EXPECT_EQ(got, "empty { a = true }\n");
}

TEST(Rewriter, SetAppendsAtTopLevel) {
  std::string got = Rewrite("a = 1", [](Rewriter& r) { return r.Set("b", Str("x")); });
  EXPECT_EQ(got, "a = 1\nb = \"x\"\n");
}

TEST(Rewriter, SetCreatesMissingChain) {
  std::string got = Rewrite("server {\n  host = \"h\"\n}\n", [](Rewriter& r) {
    return r.Set("server.tls.min_version", Str("1.3"));
  });
  EXPECT_EQ(got, "server {\n  host = \"h\"\n  tls {\n    min_version = \"1.3\"\n  }\n}\n");
}

TEST(Rewriter, SetInsertsMapEntryForm) {
  std::string got = Rewrite("labels {\n  \"team\": \"core\"\n}\n",
                            [](Rewriter& r) { return r.Set("labels.env", Str("prod")); });
  EXPECT_EQ(got, "labels {\n  \"team\": \"core\"\n  env: \"prod\"\n}\n");
}

TEST(Rewriter, SetMultilineValueIndents) {
  std::string got = Rewrite("server {\n\ttags = [\"a\"]\n}\n", [](Rewriter& r) {
    return r.Set("server.tags", List(Str("x"), Str("y")));
  });
  // The document indents with tabs, so the list body steps by a tab too.
  EXPECT_EQ(got, "server {\n\ttags = [\n\t\t\"x\",\n\t\t\"y\"\n\t]\n}\n");
}

TEST(Rewriter, RemoveWholeLineKeepsLeadingComments) {
  std::string got = Rewrite("# section\n# about a\na = 1   # trailing\nb = 2\n",
                            [](Rewriter& r) { return r.Remove("a"); });
  EXPECT_EQ(got, "# section\n# about a\nb = 2\n");
}

TEST(Rewriter, RemoveBlock) {
  std::string got = Rewrite("a = 1\nserver {\n  host = \"h\"\n}\nb = 2\n",
                            [](Rewriter& r) { return r.Remove("server"); });
  EXPECT_EQ(got, "a = 1\nb = 2\n");
}

TEST(Rewriter, RemoveInlineEntry) {
  std::string got =
      Rewrite("inline { a = 1 b = 2 }\n", [](Rewriter& r) { return r.Remove("inline.a"); });
  EXPECT_EQ(got, "inline { b = 2 }\n");
}

TEST(Rewriter, RemoveTrailingBlockComment) {
  // A /* ... */ that closes on the entry's own line goes with it.
  std::string got = Rewrite("x = 1 /* gone */\ny = 2\n", [](Rewriter& r) { return r.Remove("x"); });
  EXPECT_EQ(got, "y = 2\n");
  // A multi-line block comment is not the entry's trailing comment; only the
  // entry's exact span is removed and the comment survives.
  got = Rewrite("x = 1 /* spans\nlines */\ny = 2\n", [](Rewriter& r) { return r.Remove("x"); });
  EXPECT_EQ(got, "/* spans\nlines */\ny = 2\n");
}

TEST(Rewriter, SetRejectsNestedBadVal) {
  auto r = MustNew("xs = [1]\n");
  EXPECT_FALSE(r->Set("xs", List(Str("a"), Bad())).ok());
  auto bv = std::make_unique<BlockVal>();
  bv->entries.push_back(Assign("k", Bad()));
  EXPECT_FALSE(r->Set("m", ValuePtr(std::move(bv))).ok());
}

TEST(Rewriter, Errors) {
  auto r = MustNew("server {\n  host = \"h\"\n}\nport = 1\n");
  EXPECT_FALSE(r->Remove("nope").ok());
  EXPECT_FALSE(r->Set("server", Int("1")).ok());
  EXPECT_FALSE(r->Set("port.sub", Int("1")).ok());
  EXPECT_FALSE(r->Set("", Int("1")).ok());
  EXPECT_FALSE(r->Set("a", ValuePtr{}).ok());
  EXPECT_FALSE(Rewriter::New("port =").ok());
}

TEST(Rewriter, ConflictingEditsRejected) {
  auto r = MustNew("server {\n  host = \"h\"\n}\n");
  ASSERT_TRUE(r->Remove("server").ok());
  ASSERT_TRUE(r->Set("server.host", Str("x")).ok());
  auto out = r->Bytes();
  ASSERT_FALSE(out.ok());
  EXPECT_EQ(out.status().message(), "pxf: conflicting edits: server overlaps server.host");
}

TEST(Rewriter, SetSamePathLastWins) {
  std::string got = Rewrite("a = 1\n", [](Rewriter& r) {
    Status st = r.Set("a", Int("2"));
    return st.ok() ? r.Set("a", Int("3")) : st;
  });
  EXPECT_EQ(got, "a = 3\n");
}

struct CorpusFile {
  std::string name;
  std::string src;
};

std::vector<CorpusFile> RewriteCorpus() {
  std::vector<CorpusFile> files;
  for (const char* name : {"config.pxf", "settings.pxf"}) {
    std::ifstream in(std::string(TESTDATA_DIR) + "/differential/rewrite/" + name, std::ios::binary);
    EXPECT_TRUE(in.good()) << name;
    std::stringstream ss;
    ss << in.rdbuf();
    files.push_back({name, ss.str()});
  }
  return files;
}

TEST(Rewriter, NoEditsRoundTripsExactly) {
  for (const auto& f : RewriteCorpus()) {
    auto r = MustNew(f.src);
    EXPECT_EQ(MustBytes(*r), f.src) << f.name;
  }
}

// LeafPaths walks a document and returns the dotted path of every addressable
// leaf entry (assignments and map entries whose path segments contain no dot).
void LeafPaths(const std::string& prefix,
               const std::vector<EntryPtr>& entries,
               std::vector<std::string>* out) {
  for (const auto& e : entries) {
    if (auto* p = std::get_if<std::unique_ptr<Block>>(&e)) {
      if (!Has((*p)->name, ".")) LeafPaths(prefix + (*p)->name + ".", (*p)->entries, out);
      continue;
    }
    const std::string* key = nullptr;
    const ValuePtr* value = nullptr;
    if (auto* p = std::get_if<std::unique_ptr<Assignment>>(&e)) {
      key = &(*p)->key;
      value = &(*p)->value;
    } else if (auto* p = std::get_if<std::unique_ptr<MapEntry>>(&e)) {
      key = &(*p)->key;
      value = &(*p)->value;
    } else {
      continue;
    }
    if (Has(*key, ".")) continue;
    if (auto* bv = std::get_if<std::unique_ptr<BlockVal>>(value)) {
      LeafPaths(prefix + *key + ".", (*bv)->entries, out);
    } else {
      out->push_back(prefix + *key);
    }
  }
}

// FindLeaf resolves a leaf path against a parsed document, returning its value
// span.
std::pair<int, int> FindLeaf(const pxf::Document& doc, const std::string& path) {
  const std::vector<EntryPtr>* entries = &doc.entries;
  std::stringstream segs(path);
  std::string seg;
  std::vector<std::string> all;
  while (std::getline(segs, seg, '.')) all.push_back(seg);
  for (size_t i = 0; i < all.size(); ++i) {
    const ValuePtr* val = nullptr;
    const std::vector<EntryPtr>* next = nullptr;
    bool found = false;
    for (const auto& e : *entries) {
      if (auto* p = std::get_if<std::unique_ptr<Assignment>>(&e); p && (*p)->key == all[i]) {
        val = &(*p)->value;
      } else if (auto* p = std::get_if<std::unique_ptr<MapEntry>>(&e); p && (*p)->key == all[i]) {
        val = &(*p)->value;
      } else if (auto* p = std::get_if<std::unique_ptr<Block>>(&e); p && (*p)->name == all[i]) {
        next = &(*p)->entries;
      } else {
        continue;
      }
      found = true;
      break;
    }
    EXPECT_TRUE(found) << path << ": segment " << all[i] << " not found";
    if (!found) return {0, 0};
    if (next != nullptr) {
      entries = next;
      continue;
    }
    if (i == all.size() - 1) {
      auto [start, end] = pxf::ValueSpan(*val);
      return {start.offset, end.offset};
    }
    entries = &std::get<std::unique_ptr<BlockVal>>(*val)->entries;
  }
  ADD_FAILURE() << "path " << path << " did not terminate at a leaf";
  return {0, 0};
}

// Go's test picks one of these five with a seeded rand per path. This one runs
// all five, so it covers what any seed could pick.
TEST(Rewriter, PropertySingleEditTouchesOnlyItsSpan) {
  const std::vector<std::function<ValuePtr()>> replacements = {
      [] { return Str("edited"); },
      [] { return Int("12345"); },
      [] { return Bool(false); },
      [] { return Float("2.75"); },
      [] { return List(Int("1"), Int("2")); },
  };
  int edits = 0;
  for (const auto& f : RewriteCorpus()) {
    auto doc = pxf::Parse(f.src);
    ASSERT_TRUE(doc.ok()) << f.name;
    std::vector<std::string> paths;
    LeafPaths("", doc->entries, &paths);
    ASSERT_FALSE(paths.empty());
    for (const auto& path : paths) {
      auto [val_start, val_end] = FindLeaf(*doc, path);
      for (const auto& make : replacements) {
        auto r = MustNew(f.src);
        Status st = r->Set(path, make());
        ASSERT_TRUE(st.ok()) << f.name << ": Set(" << path << "): " << st.ToString();
        auto out = r->Bytes();
        ASSERT_TRUE(out.ok()) << f.name << ": Set(" << path << "): " << out.status().ToString();
        std::string prefix = f.src.substr(0, val_start);
        std::string suffix = f.src.substr(val_end);
        EXPECT_EQ(out->substr(0, prefix.size()), prefix) << f.name << ": " << path;
        ASSERT_GE(out->size(), suffix.size());
        EXPECT_EQ(out->substr(out->size() - suffix.size()), suffix) << f.name << ": " << path;
        // And the rewritten value must actually be the new value.
        auto redoc = pxf::Parse(*out);
        ASSERT_TRUE(redoc.ok()) << f.name << ": " << path;
        auto [ns, ne] = FindLeaf(*redoc, path);
        if (*out != f.src) {
          EXPECT_NE(out->substr(ns, ne - ns), f.src.substr(val_start, val_end - val_start))
              << f.name << ": " << path;
        }
        ++edits;
      }
    }
  }
  EXPECT_GT(edits, 0);
}

// PropertyRemoveIsContiguous checks the Remove side: removing an entry deletes
// exactly one contiguous region of the original source.
TEST(Rewriter, PropertyRemoveIsContiguous) {
  for (const auto& f : RewriteCorpus()) {
    auto doc = pxf::Parse(f.src);
    ASSERT_TRUE(doc.ok()) << f.name;
    std::vector<std::string> paths;
    LeafPaths("", doc->entries, &paths);
    for (const auto& path : paths) {
      auto r = MustNew(f.src);
      Status st = r->Remove(path);
      ASSERT_TRUE(st.ok()) << f.name << ": Remove(" << path << "): " << st.ToString();
      auto out = r->Bytes();
      ASSERT_TRUE(out.ok()) << f.name << ": Remove(" << path << ")";
      ASSERT_LT(out->size(), f.src.size()) << f.name << ": " << path;
      size_t i = 0;
      while (i < out->size() && (*out)[i] == f.src[i]) ++i;
      size_t tail = out->size() - i;
      if (tail > 0) {
        EXPECT_EQ(out->substr(i), f.src.substr(f.src.size() - tail)) << f.name << ": " << path;
      }
    }
  }
}

// --- rewrite_keyed_test.go -------------------------------------------------

// The shared fixture: a keyed collection with comments, a quoted non-identifier
// key and a dotted key, the shapes dotted-path addressing cannot reach.
const char kKeyedSrc[] = R"(id = "root"
children {
  # the welcome banner
  greeting {
    type = "Label"   # widget class
    weight = 1
  }
  "us-east-1" {
    type = "Region"
  }
  user.name {
    type = "Field"
  }
}
)";

TEST(RewriterKeyed, SetKeyedReplacesValueInElement) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.SetKeyed("children", "greeting", "weight", Int("5"));
  });
  EXPECT_EQ(got, ReplaceFirst(kKeyedSrc, "weight = 1", "weight = 5"));
}

TEST(RewriterKeyed, SetKeyedReachesQuotedAndDottedKeys) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    Status st = r.SetKeyed("children", "us-east-1", "type", Str("Zone"));
    return st.ok() ? r.SetKeyed("children", "user.name", "type", Str("Input")) : st;
  });
  EXPECT_TRUE(Has(got, "\"us-east-1\" {\n    type = \"Zone\"\n")) << got;
  EXPECT_TRUE(Has(got, "user.name {\n    type = \"Input\"\n")) << got;
}

TEST(RewriterKeyed, SetKeyedInsertsMissingFieldIntoElement) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.SetKeyed("children", "us-east-1", "weight", Int("3"));
  });
  EXPECT_TRUE(Has(got, "\"us-east-1\" {\n    type = \"Region\"\n    weight = 3\n  }")) << got;
}

TEST(RewriterKeyed, SetKeyedCreatesMissingElement) {
  std::string got = Rewrite(
      kKeyedSrc, [](Rewriter& r) { return r.SetKeyed("children", "footer", "type", Str("HBox")); });
  EXPECT_TRUE(Has(got, "  footer {\n    type = \"HBox\"\n  }\n}")) << got;
}

TEST(RewriterKeyed, SetKeyedCreatesQuotedElementAndNestedChain) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.SetKeyed("children", "eu-west-2", "geo.zone", Str("b"));
  });
  EXPECT_TRUE(Has(got, "  \"eu-west-2\" {\n    geo {\n      zone = \"b\"\n    }\n  }\n}")) << got;
}

TEST(RewriterKeyed, SetKeyedLastCallWins) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    Status st = r.SetKeyed("children", "greeting", "weight", Int("7"));
    return st.ok() ? r.SetKeyed("children", "greeting", "weight", Int("9")) : st;
  });
  EXPECT_TRUE(Has(got, "weight = 9")) << got;
  EXPECT_FALSE(Has(got, "weight = 7")) << got;
}

TEST(RewriterKeyed, SetKeyedAddressesAssignmentSpelledElement) {
  std::string got = Rewrite("children {\n  greeting = { weight = 1 }\n}\n", [](Rewriter& r) {
    return r.SetKeyed("children", "greeting", "weight", Int("4"));
  });
  EXPECT_EQ(got, "children {\n  greeting = { weight = 4 }\n}\n");
}

TEST(RewriterKeyed, RemoveKeyedFieldRemovesWholeLineWithComment) {
  std::string got =
      Rewrite(kKeyedSrc, [](Rewriter& r) { return r.RemoveKeyed("children", "greeting", "type"); });
  EXPECT_FALSE(Has(got, "widget class")) << got;
  EXPECT_FALSE(Has(got, "type = \"Label\"")) << got;
  EXPECT_TRUE(Has(got, "greeting {\n    weight = 1\n  }")) << got;
}

TEST(RewriterKeyed, RemoveKeyedElementKeepsSiblings) {
  std::string got =
      Rewrite(kKeyedSrc, [](Rewriter& r) { return r.RemoveKeyedElement("children", "greeting"); });
  EXPECT_FALSE(Has(got, "greeting")) << got;
  EXPECT_FALSE(Has(got, "Label")) << got;
  // The element's leading comment stays (it may describe the group).
  EXPECT_TRUE(Has(got, "# the welcome banner")) << got;
  EXPECT_TRUE(Has(got, "\"us-east-1\" {\n    type = \"Region\"\n  }")) << got;
}

TEST(RewriterKeyed, InsertKeyedElementAppends) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.InsertKeyedElement(
        "children", "footer", "", Body(Assign("type", Str("HBox")), Assign("weight", Int("2"))));
  });
  EXPECT_TRUE(Has(got, "  footer {\n    type = \"HBox\"\n    weight = 2\n  }\n}")) << got;
}

TEST(RewriterKeyed, InsertKeyedElementBeforeAnchor) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.InsertKeyedElement(
        "children", "header", "greeting", Body(Assign("type", Str("Banner"))));
  });
  size_t header_at = got.find("header {");
  size_t greeting_at = got.find("greeting {");
  ASSERT_NE(header_at, std::string::npos) << got;
  ASSERT_NE(greeting_at, std::string::npos) << got;
  EXPECT_LT(header_at, greeting_at) << got;
  EXPECT_TRUE(Has(got, "  header {\n    type = \"Banner\"\n  }\n")) << got;
  // The anchor's glued doc comment stays attached to the anchor.
  EXPECT_TRUE(Has(got, "  }\n  # the welcome banner\n  greeting {")) << got;
}

TEST(RewriterKeyed, InsertKeyedElementQuotedKey) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.InsertKeyedElement("children", "ap-south-1", "", Body(Assign("type", Str("Region"))));
  });
  EXPECT_TRUE(Has(got, "  \"ap-south-1\" {\n    type = \"Region\"\n  }\n}")) << got;
}

TEST(RewriterKeyed, InsertKeyedElementDuplicateIsError) {
  auto r = MustNew(kKeyedSrc);
  Status st = r->InsertKeyedElement("children", "greeting", "", {});
  EXPECT_TRUE(Has(st.message(), "already exists")) << st.message();
  // Spelling equivalence: "us-east-1" exists quoted; inserting the same unquoted
  // value collides.
  st = r->InsertKeyedElement("children", "us-east-1", "", {});
  EXPECT_TRUE(Has(st.message(), "already exists")) << st.message();
}

TEST(RewriterKeyed, RenameKeyedElementBareToBare) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.RenameKeyedElement("children", "greeting", "welcome");
  });
  EXPECT_EQ(got, ReplaceFirst(kKeyedSrc, "greeting {", "welcome {"));
}

TEST(RewriterKeyed, RenameKeyedElementQuoteHandling) {
  // Bare to quoted: the new key is not identifier-safe.
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.RenameKeyedElement("children", "greeting", "welcome-1");
  });
  EXPECT_TRUE(Has(got, "\"welcome-1\" {\n    type = \"Label\"")) << got;
  // Quoted to bare: identifier-safe names drop their quotes.
  got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.RenameKeyedElement("children", "us-east-1", "primary");
  });
  EXPECT_TRUE(Has(got, "  primary {\n    type = \"Region\"\n  }")) << got;
}

TEST(RewriterKeyed, RenameKeyedElementGuards) {
  auto r = MustNew(kKeyedSrc);
  Status st = r->RenameKeyedElement("children", "greeting", "us-east-1");
  EXPECT_TRUE(Has(st.message(), "already exists")) << st.message();
  st = r->RenameKeyedElement("children", "missing", "x");
  EXPECT_TRUE(Has(st.message(), "no element")) << st.message();
  st = r->RenameKeyedElement("children", "greeting", "");
  EXPECT_TRUE(Has(st.message(), "empty new key")) << st.message();
  // Same-key rename is a no-op, not an error.
  EXPECT_TRUE(r->RenameKeyedElement("children", "greeting", "greeting").ok());
  EXPECT_EQ(MustBytes(*r), kKeyedSrc);
}

TEST(RewriterKeyed, MoveKeyedElementToEnd) {
  std::string got = Rewrite(
      kKeyedSrc, [](Rewriter& r) { return r.MoveKeyedElement("children", "greeting", ""); });
  // The element moves verbatim, comments and trailing comment included.
  EXPECT_TRUE(Has(got,
                  "user.name {\n    type = \"Field\"\n  }\n  # the welcome banner\n  greeting {\n"
                  "    type = \"Label\"   # widget class\n    weight = 1\n  }\n}"))
      << got;
}

TEST(RewriterKeyed, MoveKeyedElementBeforeAnchor) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.MoveKeyedElement("children", "user.name", "greeting");
  });
  size_t user_at = got.find("user.name {");
  size_t greeting_at = got.find("greeting {");
  ASSERT_NE(user_at, std::string::npos) << got;
  ASSERT_NE(greeting_at, std::string::npos) << got;
  EXPECT_LT(user_at, greeting_at) << got;
}

TEST(RewriterKeyed, MoveKeyedElementNoOpBeforeSuccessor) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    return r.MoveKeyedElement("children", "greeting", "us-east-1");
  });
  EXPECT_EQ(got, kKeyedSrc);
}

TEST(RewriterKeyed, EditorsOnInlineBlock) {
  const std::string src = "children { a { x = 1 } b { x = 2 } }\n";
  std::string got = Rewrite(src, [](Rewriter& r) {
    return r.InsertKeyedElement("children", "c", "b", Body(Assign("x", Int("3"))));
  });
  EXPECT_EQ(got, "children { a { x = 1 } c { x = 3 } b { x = 2 } }\n");
  got = Rewrite(src, [](Rewriter& r) { return r.MoveKeyedElement("children", "a", ""); });
  EXPECT_EQ(got, "children { b { x = 2 } a { x = 1 } }\n");
}

TEST(RewriterKeyed, EditorsComposeInOneBatch) {
  std::string got = Rewrite(kKeyedSrc, [](Rewriter& r) {
    Status st = r.SetKeyed("children", "greeting", "weight", Int("10"));
    if (!st.ok()) return st;
    st = r.RemoveKeyedElement("children", "user.name");
    if (!st.ok()) return st;
    return r.InsertKeyedElement("children", "footer", "", Body(Assign("type", Str("HBox"))));
  });
  EXPECT_TRUE(Has(got, "weight = 10")) << got;
  EXPECT_FALSE(Has(got, "user.name")) << got;
  EXPECT_TRUE(Has(got, "footer {")) << got;
  EXPECT_TRUE(pxf::Parse(got).ok()) << got;
}

TEST(RewriterKeyed, EditorErrors) {
  auto r = MustNew(kKeyedSrc);
  EXPECT_FALSE(r->SetKeyed("nope", "k", "x", Int("1")).ok());
  EXPECT_TRUE(Has(r->SetKeyed("id", "k", "x", Int("1")).message(), "no keyed collection"));
  EXPECT_TRUE(Has(r->SetKeyed("children", "", "x", Int("1")).message(), "empty key"));
  EXPECT_TRUE(Has(r->RemoveKeyed("children", "missing", "x").message(), "no element"));
  EXPECT_TRUE(Has(r->RemoveKeyed("children", "greeting", "missing").message(), "no such entry"));
  EXPECT_TRUE(
      Has(r->MoveKeyedElement("children", "greeting", "greeting").message(), "before itself"));
  EXPECT_TRUE(
      Has(r->InsertKeyedElement("children", "x", "missing", {}).message(), "to insert before"));
}

// --- rewrite_keyed_forms_test.go -------------------------------------------

// kSplitSrc binds `children` twice; §3.13 concatenates the blocks in document
// order.
const char kSplitSrc[] = R"(children {
  a {
    type = "L"
  }
  b {
    type = "H"
  }
}
children {
  c {
    type = "V"
  }
}
)";

TEST(RewriterForms, SplitBindingFindsElementInSecondBinding) {
  std::string got = Rewrite(
      kSplitSrc, [](Rewriter& r) { return r.SetKeyed("children", "c", "type", Str("Grid")); });
  EXPECT_TRUE(Has(got, "c {\n    type = \"Grid\"\n  }")) << got;
}

TEST(RewriterForms, SplitBindingRemoveFromSecondBinding) {
  std::string got =
      Rewrite(kSplitSrc, [](Rewriter& r) { return r.RemoveKeyedElement("children", "c"); });
  EXPECT_FALSE(Has(got, "c {")) << got;
  // The now-empty second binding stays (removal is per-element).
  EXPECT_TRUE(Has(got, "a {")) << got;
  EXPECT_TRUE(Has(got, "b {")) << got;
}

TEST(RewriterForms, SplitBindingRenameAcrossBindings) {
  std::string got =
      Rewrite(kSplitSrc, [](Rewriter& r) { return r.RenameKeyedElement("children", "c", "z"); });
  EXPECT_TRUE(Has(got, "z {\n    type = \"V\"\n  }")) << got;
}

TEST(RewriterForms, SplitBindingDuplicateDetectionSpansBindings) {
  auto r = MustNew(kSplitSrc);
  // `a` lives in the first binding; inserting it (targeting the second) must
  // still be rejected.
  EXPECT_TRUE(Has(r->InsertKeyedElement("children", "a", "", {}).message(), "already exists"));
}

TEST(RewriterForms, SplitBindingAppendLandsInLastBinding) {
  std::string got = Rewrite(kSplitSrc, [](Rewriter& r) {
    return r.InsertKeyedElement("children", "d", "", Body(Assign("type", Str("X"))));
  });
  size_t first_close = got.find("}\nchildren");
  size_t d_at = got.find("d {");
  ASSERT_NE(d_at, std::string::npos) << got;
  EXPECT_GT(d_at, first_close) << got;
}

TEST(RewriterForms, SplitBindingInsertBeforeAnchorInItsBinding) {
  std::string got = Rewrite(kSplitSrc, [](Rewriter& r) {
    return r.InsertKeyedElement("children", "a2", "b", Body(Assign("type", Str("X"))));
  });
  size_t a2 = got.find("a2 {");
  size_t b = got.find("b {");
  size_t a = got.find("a {");
  EXPECT_TRUE(a < a2 && a2 < b) << got;
}

TEST(RewriterForms, SplitBindingMoveAcrossBindings) {
  std::string got = Rewrite(kSplitSrc, [](Rewriter& r) {
    // Move `c` (second binding) before `a` (first binding).
    return r.MoveKeyedElement("children", "c", "a");
  });
  size_t c = got.find("c {");
  size_t a = got.find("a {");
  ASSERT_NE(c, std::string::npos) << got;
  ASSERT_NE(a, std::string::npos) << got;
  EXPECT_LT(c, a) << got;
  EXPECT_TRUE(pxf::Parse(got).ok()) << got;
}

const char kAnonSrc[] = R"(service = "edge"
regions = [
  {
    name = "us-east-1"
    replicas = 3
  },
  {
    name = "eu-west-2"
    replicas = 1
  }
]
)";

TEST(RewriterForms, AnonRequiresKeyFieldDeclaration) {
  auto r = MustNew(kAnonSrc);
  // Without a declaration the anonymous form is not addressable.
  EXPECT_TRUE(
      Has(r->SetKeyed("regions", "us-east-1", "replicas", Int("5")).message(), "KeyedByField"));
}

TEST(RewriterForms, AnonSetKeyedEditsInPlace) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.SetKeyed("regions", "us-east-1", "replicas", Int("5"));
  });
  EXPECT_EQ(got, ReplaceFirst(kAnonSrc, "replicas = 3", "replicas = 5"));
}

TEST(RewriterForms, AnonSetKeyedInsertsMissingField) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.SetKeyed("regions", "eu-west-2", "tier", Str("edge"));
  });
  EXPECT_TRUE(Has(got, "name = \"eu-west-2\"\n    replicas = 1\n    tier = \"edge\"\n  }")) << got;
}

TEST(RewriterForms, AnonRemoveKeyedField) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.RemoveKeyed("regions", "us-east-1", "replicas");
  });
  EXPECT_FALSE(Has(got, "replicas = 3")) << got;
  EXPECT_TRUE(Has(got, "replicas = 1")) << got;
}

TEST(RewriterForms, AnonRenameRewritesKeyFieldValue) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.RenameKeyedElement("regions", "us-east-1", "us-east-2");
  });
  EXPECT_EQ(got, ReplaceFirst(kAnonSrc, "name = \"us-east-1\"", "name = \"us-east-2\""));
}

TEST(RewriterForms, AnonRemoveElementFirst) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.RemoveKeyedElement("regions", "us-east-1");
  });
  EXPECT_EQ(got, R"(service = "edge"
regions = [
  {
    name = "eu-west-2"
    replicas = 1
  }
]
)");
}

TEST(RewriterForms, AnonRemoveElementLast) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.RemoveKeyedElement("regions", "eu-west-2");
  });
  EXPECT_EQ(got, R"(service = "edge"
regions = [
  {
    name = "us-east-1"
    replicas = 3
  }
]
)");
}

TEST(RewriterForms, AnonInsertElementAppend) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.InsertKeyedElement("regions", "ap-south-1", "", Body(Assign("replicas", Int("2"))));
  });
  EXPECT_EQ(got, R"(service = "edge"
regions = [
  {
    name = "us-east-1"
    replicas = 3
  },
  {
    name = "eu-west-2"
    replicas = 1
  },
  {
    name = "ap-south-1"
    replicas = 2
  }
]
)");
}

TEST(RewriterForms, AnonInsertElementBeforeAnchor) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.InsertKeyedElement(
        "regions", "ca-central-1", "eu-west-2", Body(Assign("replicas", Int("4"))));
  });
  size_t ca = got.find("name = \"ca-central-1\"");
  size_t eu = got.find("name = \"eu-west-2\"");
  size_t us = got.find("name = \"us-east-1\"");
  EXPECT_TRUE(us < ca && ca < eu) << got;
  EXPECT_TRUE(pxf::Parse(got).ok()) << got;
}

const char kAnonSwapped[] = R"(service = "edge"
regions = [
  {
    name = "eu-west-2"
    replicas = 1
  },
  {
    name = "us-east-1"
    replicas = 3
  }
]
)";

TEST(RewriterForms, AnonMoveElement) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.MoveKeyedElement("regions", "eu-west-2", "us-east-1");
  });
  EXPECT_EQ(got, kAnonSwapped);
}

TEST(RewriterForms, AnonMoveElementToEnd) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.MoveKeyedElement("regions", "us-east-1", "");
  });
  EXPECT_EQ(got, kAnonSwapped);
}

TEST(RewriterForms, AnonMoveNoOpBeforeSuccessor) {
  std::string got = Rewrite(kAnonSrc, [](Rewriter& r) {
    r.KeyedByField("regions", "name");
    return r.MoveKeyedElement("regions", "us-east-1", "eu-west-2");
  });
  EXPECT_EQ(got, kAnonSrc);
}

TEST(RewriterForms, AnonCrossFormMoveIsError) {
  // A collection bound once as a block and once anonymously.
  auto r = MustNew(R"(children {
  a { type = "L" }
}
children = [
  {
    id = "b"
  }
]
)");
  r->KeyedByField("children", "id");
  EXPECT_TRUE(Has(r->MoveKeyedElement("children", "a", "b").message(), "different surface forms"));
  // But lookup and edit across the mixed forms works.
  EXPECT_TRUE(r->SetKeyed("children", "b", "type", Str("H")).ok());
  EXPECT_TRUE(Has(MustBytes(*r), "type = \"H\""));
}

TEST(RewriterForms, KeyedByFieldChains) {
  auto r = MustNew(kAnonSrc);
  EXPECT_EQ(&r->KeyedByField("regions", "name"), r.get());
}

// --- node_edit_test.go -----------------------------------------------------

TEST(RewriterNodeEdit, FormatValueScalars) {
  auto bytes = std::make_unique<pxf::BytesVal>();
  bytes->value = {'h', 'i'};
  auto ident = std::make_unique<pxf::IdentVal>();
  ident->name = "ENABLED";
  auto duration = std::make_unique<pxf::DurationVal>();
  duration->raw = "30s";
  auto timestamp = std::make_unique<pxf::TimestampVal>();
  timestamp->raw = "2020-01-01T00:00:00Z";
  EXPECT_EQ(pxf::FormatValue(Str("he said \"hi\"")), R"("he said \"hi\"")");
  EXPECT_EQ(pxf::FormatValue(Int("0x1F")), "0x1F");
  EXPECT_EQ(pxf::FormatValue(Float("1.5e3")), "1.5e3");
  EXPECT_EQ(pxf::FormatValue(Bool(true)), "true");
  EXPECT_EQ(pxf::FormatValue(ValuePtr(std::make_unique<pxf::NullVal>())), "null");
  EXPECT_EQ(pxf::FormatValue(ValuePtr(std::move(ident))), "ENABLED");
  EXPECT_EQ(pxf::FormatValue(ValuePtr(std::move(bytes))), "b\"aGk=\"");
  EXPECT_EQ(pxf::FormatValue(ValuePtr(std::move(duration))), "30s");
  EXPECT_EQ(pxf::FormatValue(ValuePtr(std::move(timestamp))), "2020-01-01T00:00:00Z");
}

TEST(RewriterNodeEdit, FormatValueMultiline) {
  EXPECT_EQ(pxf::FormatValue(List(Int("1"), Int("2"))), "[\n  1,\n  2\n]");
}

TEST(RewriterNodeEdit, FormatValueMatchesRewriter) {
  std::string got = Rewrite("a = 0\n", [](Rewriter& r) { return r.Set("a", Str("x\ny")); });
  EXPECT_EQ(got, "a = " + pxf::FormatValue(Str("x\ny")) + "\n");
}

TEST(RewriterNodeEdit, AppendValuePreservesDst) {
  std::string dst = "prefix=";
  pxf::AppendValue(&dst, Int("7"));
  EXPECT_EQ(dst, "prefix=7");
}

TEST(RewriterNodeEdit, SetCreatesChainWithDocumentStep) {
  std::string got = Rewrite("server {\n    host = \"h\"\n}\n", [](Rewriter& r) {
    return r.Set("server.tls.min_version", Str("1.3"));
  });
  EXPECT_EQ(got,
            "server {\n    host = \"h\"\n    tls {\n        min_version = \"1.3\"\n    }\n}\n");
}

TEST(RewriterNodeEdit, SetMultilineValueUsesDocumentStep) {
  std::string got = Rewrite("server {\n    tags = [\"a\"]\n}\n", [](Rewriter& r) {
    return r.Set("server.tags", List(Str("x"), Str("y")));
  });
  EXPECT_EQ(got, "server {\n    tags = [\n        \"x\",\n        \"y\"\n    ]\n}\n");
}

// FindAssignment returns the first assignment with the given key, walking
// nested blocks, the way a real editor would grab a node it holds.
const Assignment* FindAssignmentIn(const std::vector<EntryPtr>& entries, std::string_view key) {
  for (const auto& e : entries) {
    if (auto* a = std::get_if<std::unique_ptr<Assignment>>(&e)) {
      if ((*a)->key == key) return a->get();
    } else if (auto* b = std::get_if<std::unique_ptr<Block>>(&e)) {
      if (const Assignment* found = FindAssignmentIn((*b)->entries, key)) return found;
    }
  }
  return nullptr;
}

const Assignment& FindAssignment(const std::vector<EntryPtr>& entries, std::string_view key) {
  const Assignment* a = FindAssignmentIn(entries, key);
  if (a == nullptr) ADD_FAILURE() << "no assignment " << key;
  return *a;
}

TEST(RewriterNodeEdit, ReplaceValueTargetsSpecificSibling) {
  auto r = MustNew(R"(children {
    a { type = "Button"  text = "@one" }
    b { type = "Button"  text = "@two" }
}
)");
  // Reach the `text` assignment inside node b, which first-match paths cannot.
  const Block* children = Get<Block>(r->Document()->entries[0]);
  const Block* node_b = Get<Block>(children->entries[1]);
  const Assignment& text_b = FindAssignment(node_b->entries, "text");
  ASSERT_TRUE(r->ReplaceValue(text_b.value, Str("@changed")).ok());
  EXPECT_EQ(MustBytes(*r), R"(children {
    a { type = "Button"  text = "@one" }
    b { type = "Button"  text = "@changed" }
}
)");
}

TEST(RewriterNodeEdit, ReplaceValueReindentsMultiline) {
  auto r = MustNew("server {\n    tags = [\"a\"]\n}\n");
  const Assignment& tags = FindAssignment(r->Document()->entries, "tags");
  ASSERT_TRUE(r->ReplaceValue(tags.value, List(Str("x"), Str("y"))).ok());
  // Source indents 4 spaces, so the replacement list body steps by 4.
  EXPECT_EQ(MustBytes(*r), "server {\n    tags = [\n        \"x\",\n        \"y\"\n    ]\n}\n");
}

TEST(RewriterNodeEdit, ReplaceValueSameNodeLastWins) {
  auto r = MustNew("a = 1\n");
  const ValuePtr& v = FindAssignment(r->Document()->entries, "a").value;
  ASSERT_TRUE(r->ReplaceValue(v, Int("2")).ok());
  ASSERT_TRUE(r->ReplaceValue(v, Int("3")).ok());
  EXPECT_EQ(MustBytes(*r), "a = 3\n");
}

TEST(RewriterNodeEdit, ReplaceValueErrors) {
  auto r = MustNew("a = 1\n");
  const ValuePtr& v = FindAssignment(r->Document()->entries, "a").value;
  EXPECT_FALSE(r->ReplaceValue(ValuePtr{}, v).ok());
  EXPECT_FALSE(r->ReplaceValue(v, ValuePtr{}).ok());
  EXPECT_FALSE(r->ReplaceValue(v, Bad()).ok());
  // A hand-built value has no span.
  EXPECT_FALSE(r->ReplaceValue(Int("9"), v).ok());
}

TEST(RewriterNodeEdit, SetSpanRawSplice) {
  auto r = MustNew("a = 1\n");
  auto [start, end] = pxf::ValueSpan(FindAssignment(r->Document()->entries, "a").value);
  ASSERT_TRUE(r->SetSpan(start.offset, end.offset, "42").ok());
  EXPECT_EQ(MustBytes(*r), "a = 42\n");
}

TEST(RewriterNodeEdit, SetSpanOutOfRange) {
  auto r = MustNew("a = 1\n");
  EXPECT_FALSE(r->SetSpan(-1, 2, "x").ok());
  EXPECT_FALSE(r->SetSpan(3, 2, "x").ok());
  EXPECT_FALSE(r->SetSpan(0, 999, "x").ok());
}

TEST(RewriterNodeEdit, SetSpanCopiesText) {
  auto r = MustNew("a = 1\n");
  std::string text = "42";
  ASSERT_TRUE(r->SetSpan(4, 5, text).ok());
  text[0] = text[1] = '9';  // mutate after staging; must not affect output
  EXPECT_EQ(MustBytes(*r), "a = 42\n");
}

TEST(RewriterNodeEdit, AppendEntryMultilineMatchesSiblingIndent) {
  auto r = MustNew("server {\n    host = \"h\"\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Assign("port", Int("8080"))).ok());
  EXPECT_EQ(MustBytes(*r), "server {\n    host = \"h\"\n    port = 8080\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryBlockNode) {
  auto r = MustNew("children {\n    a { type = \"Button\" }\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Blk("b", Assign("type", Str("Label")))).ok());
  // The source steps in by 4, so the appended block's body steps by 4 too.
  EXPECT_EQ(
      MustBytes(*r),
      "children {\n    a { type = \"Button\" }\n    b {\n        type = \"Label\"\n    }\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryBlockBodyMatchesDocumentStep) {
  // A nested body must step by the document's own indent width, inferred from a
  // sibling block's body (4 spaces here), not a fixed 2.
  auto r = MustNew(
      "root {\n    children {\n        greeting {\n            type = \"Label\"\n        }\n"
      "    }\n}\n");
  const Block* children = Get<Block>(Get<Block>(r->Document()->entries[0])->entries[0]);
  ASSERT_TRUE(r->AppendEntry(children, Blk("button", Assign("type", Str("Button")))).ok());
  EXPECT_EQ(MustBytes(*r),
            "root {\n    children {\n        greeting {\n            type = \"Label\"\n        }\n"
            "        button {\n            type = \"Button\"\n        }\n    }\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryEmptyBlockUsesDocumentStep) {
  // An empty target block reveals no local step; the width is inferred
  // document-wide (4 spaces from sub's body) for the node's line and its body.
  auto r = MustNew("root {\n    sub {\n        x = 1\n    }\n    children {\n    }\n}\n");
  const Block* children = Get<Block>(Get<Block>(r->Document()->entries[0])->entries[1]);
  ASSERT_TRUE(r->AppendEntry(children, Blk("button", Assign("type", Str("Button")))).ok());
  EXPECT_EQ(MustBytes(*r),
            "root {\n    sub {\n        x = 1\n    }\n    children {\n"
            "        button {\n            type = \"Button\"\n        }\n    }\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryEmptyInlineBlock) {
  auto r = MustNew("children { }\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Blk("a", Assign("type", Str("Button")))).ok());
  EXPECT_EQ(MustBytes(*r), "children { a { type = \"Button\" } }\n");
}

TEST(RewriterNodeEdit, AppendEntryInlineBlock) {
  auto r = MustNew("inline { a = 1 }\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Assign("b", Int("2"))).ok());
  EXPECT_EQ(MustBytes(*r), "inline { a = 1 b = 2 }\n");
}

TEST(RewriterNodeEdit, AppendEntryMapForm) {
  auto r = MustNew("labels {\n  \"team\": \"core\"\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, MapE("env", Str("prod"))).ok());
  EXPECT_EQ(MustBytes(*r), "labels {\n  \"team\": \"core\"\n  env: \"prod\"\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryMultipleAccumulate) {
  auto r = MustNew("server {\n  host = \"h\"\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Assign("port", Int("1"))).ok());
  ASSERT_TRUE(r->AppendEntry(block, Assign("debug", Bool(true))).ok());
  EXPECT_EQ(MustBytes(*r), "server {\n  host = \"h\"\n  port = 1\n  debug = true\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryEmptyMultilineBlock) {
  auto r = MustNew("server {\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, Assign("port", Int("8080"))).ok());
  EXPECT_EQ(MustBytes(*r), "server {\n  port = 8080\n}\n");
}

TEST(RewriterNodeEdit, AppendEntryErrors) {
  auto r = MustNew("server {\n}\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  EXPECT_FALSE(r->AppendEntry(nullptr, Assign("a", Int("1"))).ok());
  EXPECT_FALSE(r->AppendEntry(block, EntryPtr{}).ok());
  EXPECT_FALSE(r->AppendEntry(block, Assign("a", Bad())).ok());
  // A hand-built block with no span.
  Block hand;
  hand.name = "x";
  EXPECT_FALSE(r->AppendEntry(&hand, Assign("a", Int("1"))).ok());
}

// Pins the key spelling the rewriter gives a code-built MapEntry, the decision
// FormatDocument makes: bare when the key lexes as a bare map key, quoted
// otherwise, and quoted when key_quoted says the integer-shaped key is the
// string "123".
TEST(RewriterNodeEdit, AppendEntryMapKeySpelling) {
  auto r = MustNew("labels { \"team\": \"core\" }\n");
  const Block* block = Get<Block>(r->Document()->entries[0]);
  ASSERT_TRUE(r->AppendEntry(block, MapE("404", Str("int"))).ok());
  ASSERT_TRUE(r->AppendEntry(block, MapE("123", Str("str"), true)).ok());
  ASSERT_TRUE(r->AppendEntry(block, MapE("my key", Str("q"))).ok());
  ASSERT_TRUE(r->AppendEntry(block, MapE("team", Str("again"), true)).ok());
  EXPECT_EQ(MustBytes(*r),
            "labels { \"team\": \"core\" 404: \"int\" \"123\": \"str\" \"my key\": \"q\" team: "
            "\"again\" }\n");
}

// --- this port only --------------------------------------------------------

// Go's nil is a null pointer inside the variant here, and a node may hold one
// where Go's would hold nil. The formatter writes nothing for it, as Go's type
// switch does, rather than dereferencing it.
TEST(RewriterNil, NilInsideAValueRendersAsNothing) {
  auto r = MustNew("a = 1\n");
  ASSERT_TRUE(r->Set("a", List(ValuePtr{})).ok());
  EXPECT_EQ(MustBytes(*r), "a = [\n  \n]\n");
}

// A body passed to InsertKeyedElement stays the caller's: the copy the
// Rewriter makes leaves it usable afterwards.
TEST(RewriterNil, InsertKeyedElementLeavesBodyIntact) {
  auto r = MustNew(kKeyedSrc);
  auto body = Body(Assign("type", Str("HBox")));
  ASSERT_TRUE(r->InsertKeyedElement("children", "footer", "", body).ok());
  ASSERT_EQ(body.size(), 1u);
  EXPECT_EQ(Get<Assignment>(body[0])->key, "type");
}

}  // namespace
