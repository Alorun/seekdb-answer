#include "common.h"

#include <charconv>
#include <cmath>
#include <cstdlib>

#include <unistd.h>

namespace mvdb {

void maybeCrash(const char* point) {
  const char* env = std::getenv("MVDB_CRASH_POINT");
  if (env && std::strcmp(env, point) == 0) {
    ::_exit(137);
  }
}

uint32_t crc32c(const void* data, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  const auto* p = static_cast<const unsigned char*>(data);
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

std::string encodeSchema(const Schema& schema) {
  Writer w;
  w.str(schema.name);
  w.u32(static_cast<uint32_t>(schema.columns.size()));
  for (const auto& col : schema.columns) {
    w.str(col.name);
    w.u8(static_cast<uint8_t>(col.type));
    w.u32(col.length);
  }
  w.u32(static_cast<uint32_t>(schema.pk_index));
  w.u32(static_cast<uint32_t>(schema.vector_index));
  return w.take();
}

bool decodeSchema(const std::string& payload, Schema& out, std::string& err) {
  Reader r(payload.data(), payload.size());
  out = Schema{};
  out.name = r.str();
  uint32_t n = r.u32();
  if (!r.ok() || n > 1024) {
    err = "corrupt schema column count";
    return false;
  }
  out.columns.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    out.columns[i].name = r.str();
    uint8_t t = r.u8();
    out.columns[i].length = r.u32();
    if (t < 1 || t > 3) {
      err = "corrupt schema column type";
      return false;
    }
    out.columns[i].type = static_cast<ColumnType>(t);
  }
  int32_t pk = static_cast<int32_t>(r.u32());
  int32_t vec = static_cast<int32_t>(r.u32());
  if (!r.ok() || r.remaining() != 0) {
    err = "corrupt schema payload";
    return false;
  }
  out.pk_index = pk;
  out.vector_index = vec;
  return true;
}

std::string encodeRow(const Schema& schema, const Row& row) {
  Writer w;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const Column& col = schema.columns[i];
    const Value& v = row.values[i];
    switch (col.type) {
      case ColumnType::BigInt:
        w.i64(v.i);
        break;
      case ColumnType::Varchar:
        w.str(v.s);
        break;
      case ColumnType::Vector:
        w.u32(static_cast<uint32_t>(v.v.size()));
        for (float f : v.v) w.f32(f);
        break;
    }
  }
  return w.take();
}

bool decodeRow(const Schema& schema, const std::string& payload, Row& out,
               std::string& err) {
  return decodeRow(schema, payload.data(), payload.size(), out, err);
}

bool decodeRow(const Schema& schema, const char* data, size_t size, Row& out,
               std::string& err) {
  Reader r(data, size);
  out.values.resize(schema.columns.size());
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const Column& col = schema.columns[i];
    switch (col.type) {
      case ColumnType::BigInt:
        out.values[i] = Value::MakeInt(r.i64());
        break;
      case ColumnType::Varchar:
        out.values[i] = Value::MakeStr(r.str());
        break;
      case ColumnType::Vector: {
        uint32_t n = r.u32();
        if (!r.ok() || n > 1'000'000) {
          err = "corrupt vector length";
          return false;
        }
        std::vector<float> v(n);
        for (uint32_t k = 0; k < n; ++k) v[k] = r.f32();
        out.values[i] = Value::MakeVec(std::move(v));
        break;
      }
    }
    if (!r.ok()) {
      err = "corrupt row payload";
      return false;
    }
  }
  if (r.remaining() != 0) {
    err = "trailing row payload";
    return false;
  }
  return true;
}

std::string encodeIndex(const std::string& name, uint32_t column_index) {
  Writer w;
  w.str(name);
  w.u32(column_index);
  return w.take();
}

bool decodeIndex(const std::string& payload, std::string& name,
                 uint32_t& column_index, std::string& err) {
  Reader r(payload.data(), payload.size());
  name = r.str();
  column_index = r.u32();
  if (!r.ok() || r.remaining() != 0) {
    err = "corrupt index payload";
    return false;
  }
  return true;
}

bool parseBigInt(std::string_view text, int64_t& out, std::string& err) {
  if (text.empty()) {
    err = "empty integer";
    return false;
  }
  const char* b = text.data();
  const char* e = text.data() + text.size();
  if (*b == '+') ++b;
  auto res = std::from_chars(b, e, out, 10);
  if (res.ec != std::errc() || res.ptr != e) {
    err = "invalid or out-of-range BIGINT: " + std::string(text);
    return false;
  }
  return true;
}

bool parseVectorLiteral(std::string_view text, std::vector<float>& out,
                        std::string& err) {
  out.clear();
  std::string s = trim(text);
  if (s.size() < 2 || s.front() != '[' || s.back() != ']') {
    err = "vector literal must look like [v1, v2, ...]";
    return false;
  }
  std::string body = s.substr(1, s.size() - 2);
  size_t pos = 0;
  while (pos < body.size()) {
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) {
      ++pos;
    }
    if (pos >= body.size()) break;
    const char* start = body.c_str() + pos;
    // strtod also accepts C99 hex floats (0x10); the task's vector grammar is
    // decimal only, so reject them rather than silently parsing 16.
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
      err = "invalid number in vector literal";
      return false;
    }
    // from_chars(float) is locale-independent and ~3.5x faster than strtod,
    // which matters for the 1.28M components in the 10k x 128 write path.
    const char* cursor = start;
    if (cursor < body.c_str() + body.size() && *cursor == '+') ++cursor;
    float f = 0.0f;
    auto parsed = std::from_chars(cursor, body.c_str() + body.size(), f);
    if (parsed.ec == std::errc::result_out_of_range) {
      err = "vector value overflows float32";
      return false;
    }
    if (parsed.ec != std::errc() || parsed.ptr == cursor) {
      err = "invalid number in vector literal";
      return false;
    }
    if (!std::isfinite(f)) {
      err = "vector value must be finite";
      return false;
    }
    out.push_back(f);
    pos += static_cast<size_t>(parsed.ptr - start);
    while (pos < body.size() &&
           std::isspace(static_cast<unsigned char>(body[pos]))) {
      ++pos;
    }
    if (pos < body.size()) {
      if (body[pos] != ',') {
        err = "expected ',' in vector literal";
        return false;
      }
      ++pos;
      // Reject trailing comma before ']'.
      size_t look = pos;
      while (look < body.size() &&
             std::isspace(static_cast<unsigned char>(body[look]))) {
        ++look;
      }
      if (look >= body.size()) {
        err = "trailing ',' in vector literal";
        return false;
      }
    }
  }
  return true;
}

}  // namespace mvdb
