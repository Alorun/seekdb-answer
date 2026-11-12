#pragma once

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mvdb {

// ---------------------------------------------------------------------------
// Status / error codes
// ---------------------------------------------------------------------------
struct Status {
  int code = 0;
  std::string message;

  bool ok() const { return code == 0; }
  static Status Ok() { return Status{}; }
  static Status Error(int c, std::string m) {
    return Status{c, std::move(m)};
  }
};

namespace Err {
constexpr int kSyntax = 1064;
constexpr int kTableExists = 1050;
constexpr int kTableNotFound = 1146;
constexpr int kColumnNotFound = 1054;
constexpr int kDuplicateKey = 1062;
constexpr int kColumnCount = 1136;
constexpr int kDataTooLong = 1406;
constexpr int kInvalidValue = 1366;
constexpr int kUnsupported = 1235;
constexpr int kTxnState = 1568;
constexpr int kIo = 1105;
constexpr int kIndexExists = 1061;
constexpr int kIndexNotFound = 1176;
constexpr int kUnknown = 1105;
}  // namespace Err

// ---------------------------------------------------------------------------
// Values / schema
// ---------------------------------------------------------------------------
// One cell of a row.  Rows (and the undo snapshots / top-K hits copied from
// them) own their cells, so copies stay deep and never alias table storage.
//
// The representation is 16 bytes instead of the 72 bytes a std::string +
// std::vector payload costs, which is what a row-heavy workload pays per cell:
//   * BIGINT          -> `i`
//   * VARCHAR <= 8 B  -> inline `sbuf` (the benchmark's `catNN` values)
//   * VARCHAR > 8 B   -> one heap byte block (exactly the bytes, no SSO slack)
//   * VECTOR          -> one heap float block of `len` elements
// `type` and `i` keep their original field names so call sites that only read
// the tag or the integer do not change.
class Value {
 public:
  enum class Type : uint8_t { Int = 0, Str = 1, Vec = 2 };

  Value() = default;
  Value(const Value& o) { copyFrom(o); }
  Value(Value&& o) noexcept { moveFrom(o); }
  Value& operator=(const Value& o) {
    if (this != &o) {
      release();
      copyFrom(o);
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      release();
      moveFrom(o);
    }
    return *this;
  }
  ~Value() { release(); }

  static Value MakeInt(int64_t x) {
    Value out;
    out.type = Type::Int;
    out.i = x;
    return out;
  }
  static Value MakeStr(std::string_view x) {
    Value out;
    out.type = Type::Str;
    out.assignStr(x);
    return out;
  }
  static Value MakeVec(const std::vector<float>& x) {
    return MakeVec(x.data(), x.size());
  }
  static Value MakeVec(std::vector<float>&& x) {
    return MakeVec(x.data(), x.size());
  }
  static Value MakeVec(const float* src, size_t n) {
    Value out;
    out.type = Type::Vec;
    out.assignVec(src, n);
    return out;
  }

  // Byte view of a VARCHAR payload; the data is owned by this Value.
  std::string_view str() const {
    if (type != Type::Str) return {};
    return std::string_view(own ? sp : sbuf, len);
  }
  // Element view of a VECTOR payload; the data is owned by this Value.
  std::span<const float> vec() const {
    if (type != Type::Vec) return {};
    return std::span<const float>(vp, len);
  }

  Type type = Type::Int;
  uint8_t own = 0;     // payload is a heap block owned by this Value
  uint16_t pad = 0;
  uint32_t len = 0;    // VARCHAR bytes / VECTOR elements
  union {
    int64_t i = 0;
    char* sp;
    float* vp;
    char sbuf[8];
  };

 private:
  static constexpr size_t kInline = sizeof(sbuf);

  void release() {
    if (own) {
      if (type == Type::Str) delete[] sp;
      else if (type == Type::Vec) delete[] vp;
    }
    type = Type::Int;
    own = 0;
    len = 0;
    i = 0;
  }
  void copyFrom(const Value& o) {
    type = o.type;
    len = o.len;
    own = 0;
    if (!o.own) {
      std::memcpy(static_cast<void*>(&i), static_cast<const void*>(&o.i),
                  sizeof(i));
      return;
    }
    own = 1;
    if (o.type == Type::Str) {
      sp = new char[o.len ? o.len : 1];
      if (o.len) std::memcpy(sp, o.sp, o.len);
    } else {
      vp = new float[o.len ? o.len : 1];
      if (o.len) std::memcpy(vp, o.vp, o.len * sizeof(float));
    }
  }
  void moveFrom(Value& o) noexcept {
    type = o.type;
    len = o.len;
    own = o.own;
    std::memcpy(static_cast<void*>(&i), static_cast<const void*>(&o.i),
                sizeof(i));
    o.type = Type::Int;
    o.own = 0;
    o.len = 0;
    o.i = 0;
  }
  void assignStr(std::string_view x) {
    len = static_cast<uint32_t>(x.size());
    if (x.size() <= kInline) {
      own = 0;
      if (!x.empty()) std::memcpy(sbuf, x.data(), x.size());
      return;
    }
    own = 1;
    sp = new char[x.size()];
    std::memcpy(sp, x.data(), x.size());
  }
  void assignVec(const float* src, size_t n) {
    len = static_cast<uint32_t>(n);
    own = 1;
    vp = n ? new float[n] : nullptr;
    if (n != 0) std::memcpy(vp, src, n * sizeof(float));
  }
};

static_assert(sizeof(Value) == 16, "Value must stay compact");

enum class ColumnType : uint8_t { BigInt = 1, Varchar = 2, Vector = 3 };

struct Column {
  std::string name;
  ColumnType type = ColumnType::BigInt;
  // VARCHAR: max code points; VECTOR: dimension.
  uint32_t length = 0;
};

struct Schema {
  std::string name;
  std::vector<Column> columns;
  int pk_index = -1;
  int vector_index = -1;
  uint64_t table_id = 0;
};

struct Row {
  std::vector<Value> values;
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    unsigned char x = static_cast<unsigned char>(a[i]);
    unsigned char y = static_cast<unsigned char>(b[i]);
    if (std::tolower(x) != std::tolower(y)) return false;
  }
  return true;
}

inline std::string toLower(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  return out;
}

inline std::string trim(std::string_view s) {
  size_t b = 0;
  size_t e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

// Count Unicode code points in a UTF-8 byte string. Invalid bytes count as one.
inline size_t utf8Length(std::string_view s) {
  size_t count = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) ++count;
  }
  return count;
}

inline std::string formatDouble(double v) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%.9g", v);
  return std::string(buf);
}

// ---------------------------------------------------------------------------
// CRC32C (Castagnoli), software implementation
// ---------------------------------------------------------------------------
uint32_t crc32c(const void* data, size_t n);

// Test hook: if MVDB_CRASH_POINT equals `point`, terminate immediately.
void maybeCrash(const char* point);

// ---------------------------------------------------------------------------
// Binary serialization
// ---------------------------------------------------------------------------
class Writer {
 public:
  void u8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }
  void u16(uint16_t v) {
    for (int i = 0; i < 2; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void u32(uint32_t v) {
    for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void u64(uint64_t v) {
    for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
  void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
  void str(std::string_view s) {
    u32(static_cast<uint32_t>(s.size()));
    buf_.append(s.data(), s.size());
  }
  void f32(float f) {
    uint32_t bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    u32(bits);
  }
  const std::string& data() const { return buf_; }
  std::string take() { return std::move(buf_); }

 private:
  std::string buf_;
};

class Reader {
 public:
  Reader(const char* data, size_t size) : data_(data), size_(size), pos_(0) {}

  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }
  size_t remaining() const { return size_ - pos_; }

  uint8_t u8() {
    if (!need(1)) return 0;
    return static_cast<uint8_t>(data_[pos_++]);
  }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = 0;
    for (int i = 0; i < 2; ++i) {
      v |= static_cast<uint16_t>(static_cast<unsigned char>(data_[pos_++])) << (8 * i);
    }
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast<uint32_t>(static_cast<unsigned char>(data_[pos_++])) << (8 * i);
    }
    return v;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v |= static_cast<uint64_t>(static_cast<unsigned char>(data_[pos_++])) << (8 * i);
    }
    return v;
  }
  int64_t i64() { return static_cast<int64_t>(u64()); }
  std::string str() {
    uint32_t n = u32();
    if (!ok_ || n > remaining()) {
      ok_ = false;
      return {};
    }
    std::string out(data_ + pos_, n);
    pos_ += n;
    return out;
  }
  float f32() {
    uint32_t bits = u32();
    float f = 0;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
  }

 private:
  bool need(size_t n) {
    if (!ok_ || pos_ + n > size_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  const char* data_;
  size_t size_;
  size_t pos_;
  bool ok_ = true;
};

// Schema / row / index payload encoding used by WAL and recovery.
std::string encodeSchema(const Schema& schema);
bool decodeSchema(const std::string& payload, Schema& out, std::string& err);

std::string encodeRow(const Schema& schema, const Row& row);
// Appends the same payload as encodeRow directly to an existing buffer.
void encodeRowInto(const Schema& schema, const Row& row, std::string& out);
bool decodeRow(const Schema& schema, const std::string& payload, Row& out,
               std::string& err);
bool decodeRow(const Schema& schema, const char* data, size_t size, Row& out,
               std::string& err);

// Multi-row INSERT blocks.  A single WAL header covers all `rows`; the
// payload stores columns separately and applies self-describing, lossless
// codecs (delta-varint PKs, dictionary/front-coded+RLE VARCHAR, canonical
// Huffman coding of the float sign/exponent byte).  Every codec has a raw
// fallback, so arbitrary rows round-trip exactly.
//
// Layout (little endian, varint = LEB128):
//   u8 version(=1), varint count, varint dim
//   pk block:    u8 mode (0 raw i64, 1 zigzag varint, 2 delta-zigzag varint)
//   per non-PK/non-VECTOR column in schema order:
//     BIGINT:    u8 mode (0 zigzag varint, 1 raw i64)
//     VARCHAR:   u8 mode (0 dictionary, 1 plain front-coded/RLE)
//   VECTOR:      u8 mode (0 raw float32 stream, 1 Huffman high byte + 3 planes)
bool encodeInsertBatch(const Schema& schema, const std::vector<Row>& rows,
                       std::string& out, std::string& err);
// Calls fn(pk, row) for every row in file order.  A false return from fn stops
// iteration early and is not an error (the caller signals the stop itself).
bool decodeInsertBatch(const Schema& schema, const char* data, size_t size,
                       const std::function<bool(int64_t, Row&)>& fn,
                       std::string& err);
// Fast path used by recovery: decodes only the PK block.
bool decodeInsertBatchPks(const Schema& schema, const char* data, size_t size,
                          const std::function<bool(int64_t)>& fn,
                          std::string& err);

std::string encodeIndex(const std::string& name, uint32_t column_index);
bool decodeIndex(const std::string& payload, std::string& name,
                 uint32_t& column_index, std::string& err);

// Parse a vector literal such as "[1, 0.5, -2]" into float32 values.
bool parseVectorLiteral(std::string_view text, std::vector<float>& out,
                        std::string& err);

// Parse a decimal integer with overflow checking.
bool parseBigInt(std::string_view text, int64_t& out, std::string& err);

}  // namespace mvdb
