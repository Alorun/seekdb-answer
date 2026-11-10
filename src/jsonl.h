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
  std::string s;
  std::vector<Value> array;
  std::vector<std::pair<std::string, Value>> object;

  const Value* get(std::string_view key) const;
};

bool parse(std::string_view text, Value& out, std::string& err);
std::string quote(std::string_view s);
std::string number(int64_t v);
std::string number(double v);

}  // namespace mvdb::jsonl
