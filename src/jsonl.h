#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mvdb::jsonl {

struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool b = false;
  bool is_int = false;
  int64_t i = 0;
  double d = 0;
  // String payload.  A string with no JSON escapes and no control characters
  // is not copied at all: `is_view` is set and `sv` points into the parse
  // input, which the caller keeps alive for the whole request.  Multi-row
  // INSERT payloads are ~70 KB each, so avoiding that copy (and its
  // allocation) matters on the write path.  `s` is only used when the string
  // needs unescaping; `view()` returns whichever representation is active.
  std::string s;
  std::string_view sv;
  bool is_view = false;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  std::string_view view() const {
    return is_view ? sv : std::string_view(s);
  }
  const Value* get(std::string_view key) const;
};

bool parse(std::string_view text, Value& out, std::string& err);
std::string quote(std::string_view s);
std::string number(int64_t v);
std::string number(double v);

}  // namespace mvdb::jsonl
