#include "common.h"

#include <charconv>
#include <queue>
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

void encodeRowInto(const Schema& schema, const Row& row, std::string& out) {
  auto u32 = [&](uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  auto u64 = [&](uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  };
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const Column& col = schema.columns[i];
    const Value& v = row.values[i];
    switch (col.type) {
      case ColumnType::BigInt:
        u64(static_cast<uint64_t>(v.i));
        break;
      case ColumnType::Varchar:
        u32(static_cast<uint32_t>(v.s.size()));
        out.append(v.s.data(), v.s.size());
        break;
      case ColumnType::Vector:
        u32(static_cast<uint32_t>(v.v.size()));
        for (float f : v.v) {
          uint32_t bits = 0;
          std::memcpy(&bits, &f, sizeof(bits));
          u32(bits);
        }
        break;
    }
  }
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


namespace {

// ---------------------------------------------------------------------------
// Compact multi-row INSERT blocks (see common.h for the layout).
// ---------------------------------------------------------------------------
void appendVarint(std::string& out, uint64_t v) {
  while (v >= 0x80) {
    out.push_back(static_cast<char>((v & 0x7F) | 0x80));
    v >>= 7;
  }
  out.push_back(static_cast<char>(v));
}

size_t varintSize(uint64_t v) {
  size_t n = 1;
  while (v >= 0x80) {
    v >>= 7;
    ++n;
  }
  return n;
}

uint64_t zigzag(int64_t v) {
  return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63);
}

int64_t unzigzag(uint64_t v) {
  return static_cast<int64_t>((v >> 1) ^ (0ull - (v & 1)));
}

void appendU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

struct BatchReader {
  const char* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  bool ok = true;

  bool need(size_t n) {
    if (!ok || n > size - pos) {
      ok = false;
      return false;
    }
    return true;
  }
  uint8_t u8() {
    if (!need(1)) return 0;
    return static_cast<uint8_t>(data[pos++]);
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(data[pos++])) << (8 * i);
    return v;
  }
  uint64_t u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<uint8_t>(data[pos++])) << (8 * i);
    return v;
  }
  uint64_t varint() {
    uint64_t v = 0;
    int shift = 0;
    while (true) {
      if (!need(1)) return 0;
      uint8_t b = static_cast<uint8_t>(data[pos++]);
      if (shift >= 64) {
        ok = false;
        return 0;
      }
      v |= static_cast<uint64_t>(b & 0x7F) << shift;
      if ((b & 0x80) == 0) return v;
      shift += 7;
    }
  }
};

struct HuffCode {
  uint8_t len[256] = {};
  uint32_t code[256] = {};
  int maxlen = 0;
};

bool buildHuffman(const uint32_t hist[256], HuffCode& hc) {
  struct Node {
    uint64_t freq = 0;
    int left = -1;
    int right = -1;
    int sym = -1;
  };
  struct HeapItem {
    uint64_t freq = 0;
    int node = 0;
  };
  auto cmp = [](const HeapItem& a, const HeapItem& b) {
    if (a.freq != b.freq) return a.freq > b.freq;
    return a.node > b.node;
  };
  std::priority_queue<HeapItem, std::vector<HeapItem>, decltype(cmp)> pq(cmp);
  std::vector<Node> nodes;
  nodes.reserve(512);
  for (int s = 0; s < 256; ++s) {
    if (hist[s] == 0) continue;
    nodes.push_back(Node{hist[s], -1, -1, s});
    pq.push(HeapItem{hist[s], static_cast<int>(nodes.size()) - 1});
  }
  if (nodes.empty()) return false;
  if (nodes.size() == 1) {
    hc.len[nodes[0].sym] = 1;
    hc.maxlen = 1;
  } else {
    while (pq.size() > 1) {
      HeapItem a = pq.top();
      pq.pop();
      HeapItem b = pq.top();
      pq.pop();
      nodes.push_back(Node{a.freq + b.freq, a.node, b.node, -1});
      pq.push(HeapItem{a.freq + b.freq, static_cast<int>(nodes.size()) - 1});
    }
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(pq.top().node, 0);
    while (!stack.empty()) {
      auto [idx, depth] = stack.back();
      stack.pop_back();
      const Node& n = nodes[static_cast<size_t>(idx)];
      if (n.sym >= 0) {
        int d = depth == 0 ? 1 : depth;
        if (d > 24) return false;
        hc.len[n.sym] = static_cast<uint8_t>(d);
        if (d > hc.maxlen) hc.maxlen = d;
      } else {
        stack.emplace_back(n.left, depth + 1);
        stack.emplace_back(n.right, depth + 1);
      }
    }
  }
  uint32_t bl_count[25] = {};
  for (int s = 0; s < 256; ++s) {
    if (hist[s]) ++bl_count[hc.len[s]];
  }
  uint32_t next_code[25] = {};
  uint32_t code = 0;
  for (int bits = 1; bits <= 24; ++bits) {
    code = (code + bl_count[bits - 1]) << 1;
    next_code[bits] = code;
  }
  for (int s = 0; s < 256; ++s) {
    if (hist[s]) hc.code[s] = next_code[hc.len[s]]++;
  }
  return true;
}

struct HuffDec {
  uint32_t count[25] = {};
  uint32_t first_code[25] = {};
  uint16_t first_index[25] = {};
  uint8_t syms[256] = {};
  int maxlen = 0;
};

bool buildHuffDec(const uint8_t lens[256], HuffDec& hd) {
  for (int s = 0; s < 256; ++s) {
    int l = lens[s];
    if (l == 0) continue;
    if (l > 24) return false;
    ++hd.count[l];
    if (l > hd.maxlen) hd.maxlen = l;
  }
  if (hd.maxlen == 0) return false;
  uint32_t offset[25] = {};
  uint32_t run = 0;
  for (int l = 1; l <= 24; ++l) {
    hd.first_index[l] = static_cast<uint16_t>(run);
    offset[l] = run;
    run += hd.count[l];
  }
  for (int s = 0; s < 256; ++s) {
    int l = lens[s];
    if (l) hd.syms[offset[l]++] = static_cast<uint8_t>(s);
  }
  uint32_t code = 0;
  for (int l = 1; l <= 24; ++l) {
    code = (code + hd.count[l - 1]) << 1;
    hd.first_code[l] = code;
  }
  return true;
}

// The float high byte (sign + the top exponent bits) has a very skewed
// distribution for real-world vectors; a per-batch canonical Huffman code gets
// it down to ~2.6 bits per component.  Raw mode is used when coding would
// expand the block.
struct ByteBitReader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t pos = 0;
  uint64_t acc = 0;
  int nbits = 0;
  bool ok = true;

  bool fill(int want) {
    while (nbits < want) {
      if (pos >= size) {
        ok = false;
        return false;
      }
      acc = (acc << 8) | data[pos++];
      nbits += 8;
    }
    return true;
  }
  uint32_t take(int k) {
    if (!ok) return 0;
    if (nbits < k && !fill(k)) return 0;
    nbits -= k;
    return static_cast<uint32_t>((acc >> nbits) & ((1u << k) - 1));
  }
};

size_t commonPrefix(const std::string& a, const std::string& b) {
  size_t n = std::min(a.size(), b.size());
  size_t i = 0;
  while (i < n && a[i] == b[i]) ++i;
  return i;
}

size_t rleCost(const char* p, size_t n) {
  size_t cost = 0;
  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && p[j] == p[i]) ++j;
    cost += varintSize(j - i) + 1;
    i = j;
  }
  return cost;
}

void rleWrite(std::string& out, const char* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    size_t j = i + 1;
    while (j < n && p[j] == p[i]) ++j;
    appendVarint(out, j - i);
    out.push_back(p[i]);
    i = j;
  }
}

bool encodeStringColumn(const std::vector<Row>& rows, int col, std::string& out) {
  const size_t count = rows.size();
  std::vector<uint32_t> dict_index_of(count);
  std::vector<uint32_t> prefixes(count);
  std::vector<uint32_t> suffix_lens(count);
  std::vector<uint8_t> suffix_rle(count);
  std::vector<std::string> dict;
  std::unordered_map<std::string, uint32_t> dict_index;
  dict_index.reserve(count);

  uint64_t plain_cost = 1;  // mode byte
  const std::string* prev = nullptr;
  for (size_t i = 0; i < count; ++i) {
    const std::string& s = rows[i].values[static_cast<size_t>(col)].s;
    auto it = dict_index.find(s);
    uint32_t idx = 0;
    if (it == dict_index.end()) {
      idx = static_cast<uint32_t>(dict.size());
      dict_index.emplace(s, idx);
      dict.push_back(s);
    } else {
      idx = it->second;
    }
    dict_index_of[i] = idx;
    size_t prefix = prev ? commonPrefix(*prev, s) : 0;
    size_t suffix = s.size() - prefix;
    size_t rle = rleCost(s.data() + prefix, suffix);
    uint8_t use_rle = rle < suffix ? 1 : 0;
    prefixes[i] = static_cast<uint32_t>(prefix);
    suffix_lens[i] = static_cast<uint32_t>(suffix);
    suffix_rle[i] = use_rle;
    plain_cost += varintSize(prefix) + varintSize(suffix) + 1 +
                  (use_rle ? rle : suffix);
    prev = &s;
  }

  uint64_t dict_cost = varintSize(dict.size());
  for (const std::string& s : dict) dict_cost += varintSize(s.size()) + s.size();
  for (size_t i = 0; i < count; ++i) dict_cost += varintSize(dict_index_of[i]);

  if (dict_cost + 1 < plain_cost && dict.size() < count) {
    out.push_back(1);
    appendVarint(out, dict.size());
    for (const std::string& s : dict) {
      appendVarint(out, s.size());
      out.append(s);
    }
    for (size_t i = 0; i < count; ++i) appendVarint(out, dict_index_of[i]);
    return true;
  }

  out.push_back(0);
  for (size_t i = 0; i < count; ++i) {
    const std::string& s = rows[i].values[static_cast<size_t>(col)].s;
    appendVarint(out, prefixes[i]);
    appendVarint(out, suffix_lens[i]);
    out.push_back(static_cast<char>(suffix_rle[i]));
    if (suffix_rle[i]) {
      rleWrite(out, s.data() + prefixes[i], suffix_lens[i]);
    } else {
      out.append(s.data() + prefixes[i], suffix_lens[i]);
    }
  }
  return true;
}

bool encodeBigIntColumn(const std::vector<Row>& rows, int col, std::string& out) {
  uint64_t var_cost = 0;
  for (const Row& row : rows) var_cost += varintSize(zigzag(row.values[static_cast<size_t>(col)].i));
  if (var_cost < rows.size() * 8) {
    out.push_back(0);
    for (const Row& row : rows) appendVarint(out, zigzag(row.values[static_cast<size_t>(col)].i));
  } else {
    out.push_back(1);
    for (const Row& row : rows) {
      appendU64(out, static_cast<uint64_t>(row.values[static_cast<size_t>(col)].i));
    }
  }
  return true;
}

bool encodePkBlock(const std::vector<int64_t>& pks, std::string& out) {
  uint64_t raw_cost = pks.size() * 8;
  uint64_t abs_cost = 0;
  uint64_t delta_cost = 0;
  int64_t prev = 0;
  for (int64_t pk : pks) {
    abs_cost += varintSize(zigzag(pk));
    int64_t diff = static_cast<int64_t>(static_cast<uint64_t>(pk) - static_cast<uint64_t>(prev));
    delta_cost += varintSize(zigzag(diff));
    prev = pk;
  }
  uint8_t mode = 0;
  uint64_t best = raw_cost;
  if (abs_cost < best) {
    best = abs_cost;
    mode = 1;
  }
  if (delta_cost < best) {
    best = delta_cost;
    mode = 2;
  }
  out.push_back(static_cast<char>(mode));
  if (mode == 0) {
    for (int64_t pk : pks) appendU64(out, static_cast<uint64_t>(pk));
  } else if (mode == 1) {
    for (int64_t pk : pks) appendVarint(out, zigzag(pk));
  } else {
    prev = 0;
    for (int64_t pk : pks) {
      int64_t diff = static_cast<int64_t>(static_cast<uint64_t>(pk) - static_cast<uint64_t>(prev));
      appendVarint(out, zigzag(diff));
      prev = pk;
    }
  }
  return true;
}

bool readPkBlock(BatchReader& r, size_t count, std::vector<int64_t>& pks,
                 std::string& err) {
  uint8_t mode = r.u8();
  if (!r.ok) {
    err = "truncated batch pk block";
    return false;
  }
  pks.resize(count);
  if (mode == 0) {
    if (!r.need(count * 8)) {
      err = "truncated batch pk block";
      return false;
    }
    for (size_t i = 0; i < count; ++i) pks[i] = static_cast<int64_t>(r.u64());
  } else if (mode == 1) {
    for (size_t i = 0; i < count; ++i) pks[i] = unzigzag(r.varint());
  } else if (mode == 2) {
    int64_t prev = 0;
    for (size_t i = 0; i < count; ++i) {
      int64_t diff = unzigzag(r.varint());
      prev = static_cast<int64_t>(static_cast<uint64_t>(prev) + static_cast<uint64_t>(diff));
      pks[i] = prev;
    }
  } else {
    err = "corrupt batch pk mode";
    return false;
  }
  if (!r.ok) {
    err = "truncated batch pk block";
    return false;
  }
  return true;
}

bool encodeVectorBlock(const Schema& schema, const std::vector<Row>& rows, int col,
                       size_t dim, std::string& out, std::string& err) {
  (void)schema;
  const size_t n = rows.size() * dim;
  std::vector<uint32_t> bits(n);
  uint32_t hist[256] = {};
  size_t k = 0;
  for (const Row& row : rows) {
    const std::vector<float>& v = row.values[static_cast<size_t>(col)].v;
    if (v.size() != dim) {
      err = "vector dimension mismatch in batch";
      return false;
    }
    for (float f : v) {
      uint32_t b = 0;
      std::memcpy(&b, &f, sizeof(b));
      bits[k++] = b;
      ++hist[b >> 24];
    }
  }

  HuffCode hc;
  bool use_huffman = n > 0 && buildHuffman(hist, hc);
  if (use_huffman) {
    uint64_t total = 256ull * 8 + 4;
    for (int s = 0; s < 256; ++s) {
      if (hist[s]) total += static_cast<uint64_t>(hist[s]) * hc.len[s];
    }
    if (total >= static_cast<uint64_t>(n) * 8) use_huffman = false;
  }
  if (!use_huffman) {
    out.push_back(0);
    const size_t base = out.size();
    out.resize(base + n * 4);
    char* dst = out.data() + base;
    for (size_t i = 0; i < n; ++i) {
      uint32_t b = bits[i];
      dst[4 * i] = static_cast<char>(b & 0xFF);
      dst[4 * i + 1] = static_cast<char>((b >> 8) & 0xFF);
      dst[4 * i + 2] = static_cast<char>((b >> 16) & 0xFF);
      dst[4 * i + 3] = static_cast<char>((b >> 24) & 0xFF);
    }
    return true;
  }

  out.push_back(1);
  for (int s = 0; s < 256; ++s) out.push_back(static_cast<char>(hc.len[s]));
  const size_t size_pos = out.size();
  out.append(4, '\0');
  const size_t start = out.size();
  uint64_t acc = 0;
  int nbits = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t hi = static_cast<uint8_t>(bits[i] >> 24);
    int len = hc.len[hi];
    acc = (acc << len) | hc.code[hi];
    nbits += len;
    if (nbits >= 32) {
      nbits -= 32;
      uint32_t word = static_cast<uint32_t>(acc >> nbits);
      out.push_back(static_cast<char>(word & 0xFF));
      out.push_back(static_cast<char>((word >> 8) & 0xFF));
      out.push_back(static_cast<char>((word >> 16) & 0xFF));
      out.push_back(static_cast<char>((word >> 24) & 0xFF));
    }
  }
  while (nbits >= 8) {
    nbits -= 8;
    out.push_back(static_cast<char>((acc >> nbits) & 0xFF));
  }
  if (nbits > 0) out.push_back(static_cast<char>((acc << (8 - nbits)) & 0xFF));
  const size_t coded = out.size() - start;
  if (coded > 0xFFFFFFFFull) {
    err = "batch huffman block too large";
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    out[size_pos + static_cast<size_t>(i)] = static_cast<char>((coded >> (8 * i)) & 0xFF);
  }
  const size_t base = out.size();
  out.resize(base + n * 3);
  char* dst = out.data() + base;
  for (size_t i = 0; i < n; ++i) {
    uint32_t b = bits[i];
    dst[i] = static_cast<char>(b & 0xFF);
    dst[i + n] = static_cast<char>((b >> 8) & 0xFF);
    dst[i + 2 * n] = static_cast<char>((b >> 16) & 0xFF);
  }
  return true;
}

bool decodeStringColumn(BatchReader& r, int col, std::vector<Row>& rows,
                        std::string& err) {
  uint8_t mode = r.u8();
  if (!r.ok) {
    err = "truncated batch string block";
    return false;
  }
  const size_t count = rows.size();
  if (mode == 0) {
    const std::string* prev = nullptr;
    for (size_t i = 0; i < count; ++i) {
      uint64_t prefix = r.varint();
      uint64_t suffix = r.varint();
      uint8_t flags = r.u8();
      if (!r.ok || (prev == nullptr ? prefix != 0 : prefix > prev->size())) {
        err = "corrupt batch string block";
        return false;
      }
      std::string s = prev ? prev->substr(0, static_cast<size_t>(prefix)) : std::string();
      if (flags == 0) {
        if (!r.need(static_cast<size_t>(suffix))) {
          err = "truncated batch string";
          return false;
        }
        s.append(r.data + r.pos, static_cast<size_t>(suffix));
        r.pos += static_cast<size_t>(suffix);
      } else if (flags == 1) {
        uint64_t left = suffix;
        while (left > 0) {
          uint64_t run = r.varint();
          uint8_t byte = r.u8();
          if (!r.ok || run == 0 || run > left) {
            err = "corrupt batch string rle";
            return false;
          }
          s.append(static_cast<size_t>(run), static_cast<char>(byte));
          left -= run;
        }
      } else {
        err = "corrupt batch string mode";
        return false;
      }
      rows[i].values[static_cast<size_t>(col)] = Value::MakeStr(std::move(s));
      prev = &rows[i].values[static_cast<size_t>(col)].s;
    }
    return true;
  }
  if (mode == 1) {
    uint64_t ndict = r.varint();
    if (!r.ok || ndict > count) {
      err = "corrupt batch string dictionary";
      return false;
    }
    std::vector<std::string> dict(static_cast<size_t>(ndict));
    for (size_t k = 0; k < dict.size(); ++k) {
      uint64_t len = r.varint();
      if (!r.ok || !r.need(static_cast<size_t>(len))) {
        err = "truncated batch string dictionary";
        return false;
      }
      dict[k].assign(r.data + r.pos, static_cast<size_t>(len));
      r.pos += static_cast<size_t>(len);
    }
    for (size_t i = 0; i < count; ++i) {
      uint64_t idx = r.varint();
      if (!r.ok || idx >= ndict) {
        err = "corrupt batch string dictionary index";
        return false;
      }
      rows[i].values[static_cast<size_t>(col)] = Value::MakeStr(dict[static_cast<size_t>(idx)]);
    }
    return true;
  }
  err = "corrupt batch string mode";
  return false;
}

bool decodeBigIntColumn(BatchReader& r, int col, std::vector<Row>& rows,
                        std::string& err) {
  uint8_t mode = r.u8();
  if (!r.ok) {
    err = "truncated batch integer block";
    return false;
  }
  const size_t count = rows.size();
  if (mode == 0) {
    for (size_t i = 0; i < count; ++i) {
      rows[i].values[static_cast<size_t>(col)] = Value::MakeInt(unzigzag(r.varint()));
    }
  } else if (mode == 1) {
    for (size_t i = 0; i < count; ++i) {
      rows[i].values[static_cast<size_t>(col)] = Value::MakeInt(static_cast<int64_t>(r.u64()));
    }
  } else {
    err = "corrupt batch integer mode";
    return false;
  }
  if (!r.ok) {
    err = "truncated batch integer block";
    return false;
  }
  return true;
}

bool decodeVectorBlock(BatchReader& r, const Schema& schema, int col, size_t count,
                       size_t dim, std::vector<Row>& rows, std::string& err) {
  (void)schema;
  const size_t n = count * dim;
  uint8_t mode = r.u8();
  if (!r.ok) {
    err = "truncated batch vector block";
    return false;
  }
  if (mode == 0) {
    if (!r.need(n * 4)) {
      err = "truncated batch vectors";
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      std::vector<float> v(dim);
      for (size_t j = 0; j < dim; ++j) {
        uint32_t bits = 0;
        for (int k = 0; k < 4; ++k) {
          bits |= static_cast<uint32_t>(static_cast<uint8_t>(r.data[r.pos++])) << (8 * k);
        }
        std::memcpy(&v[j], &bits, sizeof(bits));
      }
      rows[i].values[static_cast<size_t>(col)] = Value::MakeVec(std::move(v));
    }
    return true;
  }
  if (mode != 1) {
    err = "corrupt batch vector mode";
    return false;
  }
  uint8_t lens[256];
  for (int s = 0; s < 256; ++s) lens[s] = r.u8();
  uint32_t coded = r.u32();
  if (!r.ok || coded > r.size - r.pos) {
    err = "truncated batch vector stream";
    return false;
  }
  HuffDec hd;
  if (!buildHuffDec(lens, hd)) {
    err = "corrupt batch huffman table";
    return false;
  }
  std::vector<uint8_t> hi(n);
  ByteBitReader br{reinterpret_cast<const uint8_t*>(r.data + r.pos), coded};
  for (size_t i = 0; i < n; ++i) {
    uint32_t code = 0;
    int sym = -1;
    for (int len = 1; len <= hd.maxlen; ++len) {
      int bit = br.take(1);
      if (!br.ok) {
        err = "truncated batch huffman stream";
        return false;
      }
      code = (code << 1) | static_cast<uint32_t>(bit);
      uint32_t first = hd.first_code[len];
      if (code >= first && code - first < hd.count[len]) {
        sym = hd.syms[hd.first_index[len] + (code - first)];
        break;
      }
    }
    if (sym < 0) {
      err = "corrupt batch huffman symbol";
      return false;
    }
    hi[i] = static_cast<uint8_t>(sym);
  }
  r.pos += coded;
  if (!r.need(n * 3)) {
    err = "truncated batch vector planes";
    return false;
  }
  const char* planes = r.data + r.pos;
  for (size_t i = 0; i < count; ++i) {
    std::vector<float> v(dim);
    for (size_t j = 0; j < dim; ++j) {
      size_t idx = i * dim + j;
      uint32_t bits = static_cast<uint32_t>(hi[idx]) << 24;
      bits |= static_cast<uint32_t>(static_cast<uint8_t>(planes[idx + n])) << 8;
      bits |= static_cast<uint32_t>(static_cast<uint8_t>(planes[idx + 2 * n])) << 16;
      bits |= static_cast<uint32_t>(static_cast<uint8_t>(planes[idx]));
      std::memcpy(&v[j], &bits, sizeof(bits));
    }
    rows[i].values[static_cast<size_t>(col)] = Value::MakeVec(std::move(v));
  }
  r.pos += n * 3;
  return true;
}

}  // namespace

bool encodeInsertBatch(const Schema& schema, const std::vector<Row>& rows,
                       std::string& out, std::string& err) {
  if (rows.empty()) {
    err = "cannot encode an empty insert batch";
    return false;
  }
  if (schema.pk_index < 0) {
    err = "insert batch requires a primary key";
    return false;
  }
  const int pk_col = schema.pk_index;
  const int vec_col = schema.vector_index;
  const size_t dim = vec_col >= 0 ? schema.columns[static_cast<size_t>(vec_col)].length : 0;
  if (dim > 1000000) {
    err = "invalid vector dimension";
    return false;
  }
  std::vector<int64_t> pks;
  pks.reserve(rows.size());
  for (const Row& row : rows) {
    const Value& pk = row.values[static_cast<size_t>(pk_col)];
    if (pk.type != Value::Type::Int) {
      err = "primary key is not an integer";
      return false;
    }
    pks.push_back(pk.i);
  }

  out.push_back(1);  // payload version
  appendVarint(out, rows.size());
  appendVarint(out, dim);
  encodePkBlock(pks, out);
  for (size_t c = 0; c < schema.columns.size(); ++c) {
    if (static_cast<int>(c) == pk_col || static_cast<int>(c) == vec_col) continue;
    const Column& col = schema.columns[c];
    if (col.type == ColumnType::BigInt) {
      encodeBigIntColumn(rows, static_cast<int>(c), out);
    } else if (col.type == ColumnType::Varchar) {
      encodeStringColumn(rows, static_cast<int>(c), out);
    } else {
      err = "unexpected column type in batch";
      return false;
    }
  }
  if (vec_col >= 0) {
    return encodeVectorBlock(schema, rows, vec_col, dim, out, err);
  }
  return true;
}

namespace {

bool readBatchHeader(BatchReader& r, const Schema& schema, size_t& count,
                     size_t& dim, std::string& err) {
  uint8_t version = r.u8();
  if (!r.ok || version != 1) {
    err = "unsupported insert batch version";
    return false;
  }
  uint64_t n = r.varint();
  uint64_t d = r.varint();
  if (!r.ok || n == 0 || n > 10000000 || n > r.size) {
    err = "corrupt insert batch header";
    return false;
  }
  if (schema.vector_index >= 0) {
    if (d != schema.columns[static_cast<size_t>(schema.vector_index)].length ||
        d > 1000000) {
      err = "batch vector dimension does not match schema";
      return false;
    }
  } else if (d != 0) {
    err = "unexpected vector data in batch";
    return false;
  }
  count = static_cast<size_t>(n);
  dim = static_cast<size_t>(d);
  return true;
}

}  // namespace

bool decodeInsertBatch(const Schema& schema, const char* data, size_t size,
                       const std::function<bool(int64_t, Row&)>& fn,
                       std::string& err) {
  BatchReader r{data, size, 0, true};
  size_t count = 0;
  size_t dim = 0;
  if (!readBatchHeader(r, schema, count, dim, err)) return false;
  std::vector<int64_t> pks;
  if (!readPkBlock(r, count, pks, err)) return false;
  if (schema.pk_index < 0) {
    err = "batch schema has no primary key";
    return false;
  }
  std::vector<Row> rows(count);
  for (size_t i = 0; i < count; ++i) {
    rows[i].values.resize(schema.columns.size());
    rows[i].values[static_cast<size_t>(schema.pk_index)] = Value::MakeInt(pks[i]);
  }
  for (size_t c = 0; c < schema.columns.size(); ++c) {
    if (static_cast<int>(c) == schema.pk_index || static_cast<int>(c) == schema.vector_index) {
      continue;
    }
    const Column& col = schema.columns[c];
    if (col.type == ColumnType::BigInt) {
      if (!decodeBigIntColumn(r, static_cast<int>(c), rows, err)) return false;
    } else if (col.type == ColumnType::Varchar) {
      if (!decodeStringColumn(r, static_cast<int>(c), rows, err)) return false;
    } else {
      err = "unexpected column type in batch";
      return false;
    }
  }
  if (schema.vector_index >= 0) {
    if (!decodeVectorBlock(r, schema, schema.vector_index, count, dim, rows, err)) {
      return false;
    }
  }
  if (!r.ok || r.pos != r.size) {
    err = "trailing bytes in insert batch";
    return false;
  }
  for (size_t i = 0; i < count; ++i) {
    if (!fn(pks[i], rows[i])) break;
  }
  return true;
}

bool decodeInsertBatchPks(const Schema& schema, const char* data, size_t size,
                          const std::function<bool(int64_t)>& fn,
                          std::string& err) {
  BatchReader r{data, size, 0, true};
  size_t count = 0;
  size_t dim = 0;
  if (!readBatchHeader(r, schema, count, dim, err)) return false;
  std::vector<int64_t> pks;
  if (!readPkBlock(r, count, pks, err)) return false;
  for (int64_t pk : pks) {
    if (!fn(pk)) break;
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
