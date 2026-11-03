// SPDX-License-Identifier: MIT
// Copyright (c) 2026 TrendVidia, LLC.
//
// Name conversions shared by XMLToProto and ProtoToXML, as in protowire-go's
// xmlschema.go. Go classifies runes with package unicode; these classify ASCII
// only. The two agree on every name either converter can use: proto
// identifiers are ASCII, so a non-ASCII SBE name gives a .proto no compiler
// accepts, and ProtoToXML only ever sees proto identifiers.

#pragma once

#include <string>
#include <string_view>

namespace protowire::sbe::xml_names {

inline bool IsUpper(char c) {
  return c >= 'A' && c <= 'Z';
}
inline bool IsLower(char c) {
  return c >= 'a' && c <= 'z';
}
inline char ToLower(char c) {
  return IsUpper(c) ? static_cast<char>(c - 'A' + 'a') : c;
}
inline char ToUpper(char c) {
  return IsLower(c) ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string CamelToSnake(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    const char r = s[i];
    if (IsUpper(r)) {
      if (i > 0) {
        if (IsLower(s[i - 1])) {
          out += '_';
        } else if (i + 1 < s.size() && IsLower(s[i + 1])) {
          out += '_';
        }
      }
      out += ToLower(r);
    } else {
      out += r;
    }
  }
  return out;
}

inline std::string SnakeToCamel(std::string_view s) {
  std::string out;
  size_t part = 0;
  size_t start = 0;
  while (true) {
    const size_t end = s.find('_', start);
    std::string_view p = s.substr(start, end == std::string_view::npos ? s.npos : end - start);
    if (!p.empty()) {
      if (part == 0) {
        out += p;
      } else {
        out += ToUpper(p[0]);
        out += p.substr(1);
      }
    }
    ++part;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return out;
}

inline std::string CamelToScreamingSnake(std::string_view s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (IsUpper(s[i]) && i > 0 && IsLower(s[i - 1])) out += '_';
    out += ToUpper(s[i]);
  }
  return out;
}

inline std::string ScreamingSnakeToPascal(std::string_view s) {
  std::string out;
  size_t start = 0;
  while (true) {
    const size_t end = s.find('_', start);
    std::string_view p = s.substr(start, end == std::string_view::npos ? s.npos : end - start);
    if (!p.empty()) {
      out += ToUpper(p[0]);
      for (char c : p.substr(1)) out += ToLower(c);
    }
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return out;
}

inline std::string StripEnumPrefix(std::string_view value_name, std::string_view enum_name) {
  const std::string prefix = CamelToScreamingSnake(enum_name) + "_";
  if (value_name.substr(0, prefix.size()) == prefix) {
    return ScreamingSnakeToPascal(value_name.substr(prefix.size()));
  }
  return ScreamingSnakeToPascal(value_name);
}

inline std::string SingularPascal(std::string s) {
  if (s.empty()) return s;
  const auto ends = [&s](std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
  };
  if (ends("ies") && s.size() > 3) {
    s = s.substr(0, s.size() - 3) + "y";
  } else if (ends("s") && !ends("ss") && s.size() > 1) {
    s.pop_back();
  }
  s[0] = ToUpper(s[0]);
  return s;
}

}  // namespace protowire::sbe::xml_names
