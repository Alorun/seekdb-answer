#include "jsonl.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace mvdb::jsonl {

const Value* Value::get(std::string_view key) const {
  for (const auto& [k, v] : object) {
    if (k == key) return &v;
  }
  return nullptr;
}

namespace {

struct Parser {
  std::string_view s;
  size_t p = 0;
  std::string err;

  void ws() {
    while (p < s.size() &&
           (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) {
      ++p;
    }
  }

  bool fail(const char* m) {
    if (err.empty()) err = std::string(m) + " at byte " + std::to_string(p);
    return false;
  }

  static void utf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) {
      out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
      out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
      out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
      out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
  }

  bool hex4(uint32_t& v) {
    if (p + 4 > s.size()) return fail("short unicode escape");
    v = 0;
    for (int k = 0; k < 4; ++k) {
      char c = s[p++];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
      else return fail("bad unicode escape");
    }
    return true;
  }

  bool string(std::string& out) {
    if (p >= s.size() || s[p] != '"') return fail("expected string");
    ++p;
    while (p < s.size()) {
      unsigned char c = static_cast<unsigned char>(s[p++]);
      if (c == '"') return true;
      if (c < 0x20) return fail("control character in string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        continue;
      }
      if (p >= s.size()) return fail("short escape");
      char e = s[p++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = 0;
          if (!hex4(cp)) return false;
          if (cp >= 0xd800 && cp <= 0xdbff) {
            if (p + 2 > s.size() || s[p] != '\\' || s[p + 1] != 'u') {
              return fail("missing low surrogate");
            }
            p += 2;
            uint32_t lo = 0;
            if (!hex4(lo) || lo < 0xdc00 || lo > 0xdfff) {
              return fail("bad low surrogate");
            }
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          }
          utf8(out, cp);
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }

  bool number(Value& v) {
    size_t start = p;
    if (p < s.size() && s[p] == '-') ++p;
    if (p >= s.size() || s[p] < '0' || s[p] > '9') return fail("bad number");
    if (s[p] == '0') {
      ++p;
    } else {
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    bool integral = true;
    if (p < s.size() && s[p] == '.') {
      integral = false;
      ++p;
      if (p >= s.size() || s[p] < '0' || s[p] > '9') return fail("bad fraction");
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      integral = false;
      ++p;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) ++p;
      if (p >= s.size() || s[p] < '0' || s[p] > '9') return fail("bad exponent");
      while (p < s.size() && s[p] >= '0' && s[p] <= '9') ++p;
    }
    std::string_view tok = s.substr(start, p - start);
    v.type = Value::Type::Number;
    if (integral) {
      int64_t x = 0;
      auto r = std::from_chars(tok.data(), tok.data() + tok.size(), x);
      if (r.ec == std::errc() && r.ptr == tok.data() + tok.size()) {
        v.is_int = true;
        v.i = x;
        return true;
      }
    }
    std::string tmp(tok);
    char* end = nullptr;
    v.d = std::strtod(tmp.c_str(), &end);
    if (end != tmp.c_str() + tmp.size() || !std::isfinite(v.d)) {
      return fail("non-finite number");
    }
    v.is_int = false;
    return true;
  }

  bool value(Value& v) {
    ws();
    if (p >= s.size()) return fail("unexpected end");
    char c = s[p];
    if (c == '{') {
      ++p;
      v.type = Value::Type::Object;
      ws();
      if (p < s.size() && s[p] == '}') {
        ++p;
        return true;
      }
      while (true) {
        ws();
        std::string key;
        if (!string(key)) return false;
        ws();
        if (p >= s.size() || s[p] != ':') return fail("expected colon");
        ++p;
        Value child;
        if (!value(child)) return false;
        v.object.emplace_back(std::move(key), std::move(child));
        ws();
        if (p < s.size() && s[p] == ',') {
          ++p;
          continue;
        }
        if (p < s.size() && s[p] == '}') {
          ++p;
          return true;
        }
        return fail("expected comma/end object");
      }
    }
    if (c == '[') {
      ++p;
      v.type = Value::Type::Array;
      ws();
      if (p < s.size() && s[p] == ']') {
        ++p;
        return true;
      }
      while (true) {
        Value child;
        if (!value(child)) return false;
        v.array.push_back(std::move(child));
        ws();
        if (p < s.size() && s[p] == ',') {
          ++p;
          continue;
        }
        if (p < s.size() && s[p] == ']') {
          ++p;
          return true;
        }
        return fail("expected comma/end array");
      }
    }
    if (c == '"') {
      v.type = Value::Type::String;
      return string(v.s);
    }
    if (c == 't' && s.substr(p, 4) == "true") {
      p += 4;
      v.type = Value::Type::Bool;
      v.b = true;
      return true;
    }
    if (c == 'f' && s.substr(p, 5) == "false") {
      p += 5;
      v.type = Value::Type::Bool;
      v.b = false;
      return true;
    }
    if (c == 'n' && s.substr(p, 4) == "null") {
      p += 4;
      v.type = Value::Type::Null;
      return true;
    }
    return number(v);
  }
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string& err) {
  Parser p{text};
  out = Value{};
  if (!p.value(out)) {
    err = p.err.empty() ? "invalid JSON" : p.err;
    return false;
  }
  p.ws();
  if (p.p != text.size()) {
    err = "trailing JSON text at byte " + std::to_string(p.p);
    return false;
  }
  return true;
}

std::string quote(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('"');
  static const char* hex = "0123456789abcdef";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out.push_back(hex[(c >> 4) & 0xf]);
          out.push_back(hex[c & 0xf]);
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
  return out;
}

std::string number(int64_t v) { return std::to_string(v); }

std::string number(double v) {
  if (!std::isfinite(v)) return "0";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return buf;
}

}  // namespace mvdb::jsonl
