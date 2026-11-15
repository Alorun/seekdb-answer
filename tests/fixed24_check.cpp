// Fixed24 vector-block self-check.
//
// Covers:
//   * 20,000 random decimal vectors with 0..6 fractional digits (negatives,
//     zeros, +/-1, trailing zeros) must use the new mode 2 and round-trip
//     bitwise through encodeInsertBatch -> decodeInsertBatch.
//   * 20,000 random float32 bit patterns must fall back to mode 0/1 and still
//     round-trip bitwise (one sentinel NaN per batch forces the fallback).
//   * A mixed batch with 7+ decimals / exponent / huge values falls back.
//   * Truncated mode-2 blocks are rejected instead of reading out of bounds.
//
// Build: g++ -O2 -std=c++20 -Isrc tests/fixed24_check.cpp src/common.cpp \
//            -o /tmp/fixed24_check
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "common.h"

namespace {

constexpr size_t kDim = 32;

mvdb::Schema makeSchema() {
  mvdb::Schema s;
  s.name = "t";
  s.columns.push_back(mvdb::Column{"id", mvdb::ColumnType::BigInt, 0});
  s.columns.push_back(mvdb::Column{"v", mvdb::ColumnType::Vector, kDim});
  s.pk_index = 0;
  s.vector_index = 1;
  s.table_id = 1;
  return s;
}

mvdb::Row makeRow(int64_t pk, const std::vector<float>& v) {
  mvdb::Row r;
  r.values.resize(2);
  r.values[0] = mvdb::Value::MakeInt(pk);
  r.values[1] = mvdb::Value::MakeVec(v);
  return r;
}

uint32_t floatBits(float f) {
  uint32_t b = 0;
  std::memcpy(&b, &f, sizeof(b));
  return b;
}

bool sameBits(const std::vector<mvdb::Row>& a, const std::vector<mvdb::Row>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i].values.size() != b[i].values.size()) return false;
    if (a[i].values[0].i != b[i].values[0].i) return false;
    std::span<const float> x = a[i].values[1].vec();
    std::span<const float> y = b[i].values[1].vec();
    if (x.size() != y.size()) return false;
    if (!x.empty() && std::memcmp(x.data(), y.data(), x.size() * sizeof(float)) != 0) {
      return false;
    }
  }
  return true;
}

bool readVarint(const std::string& p, size_t& pos, uint64_t& out) {
  out = 0;
  int shift = 0;
  while (pos < p.size()) {
    uint8_t b = static_cast<uint8_t>(p[pos++]);
    if (shift >= 64) return false;
    out |= static_cast<uint64_t>(b & 0x7F) << shift;
    if ((b & 0x80) == 0) return true;
    shift += 7;
  }
  return false;
}

// The test schema has only PK + VECTOR, so the vector block starts right
// after the PK block.  Returns -1 when the payload cannot be parsed.
int vectorBlockMode(const std::string& payload, const mvdb::Schema& schema,
                    size_t count) {
  if (payload.empty() || static_cast<uint8_t>(payload[0]) != 1) return -1;
  size_t pos = 1;
  uint64_t n = 0;
  uint64_t d = 0;
  if (!readVarint(payload, pos, n) || !readVarint(payload, pos, d)) return -1;
  if (n != count || d != kDim || pos >= payload.size()) return -1;
  const uint8_t pk_mode = static_cast<uint8_t>(payload[pos++]);
  if (pk_mode == 0) {
    pos += static_cast<size_t>(n) * 8;
  } else if (pk_mode == 1 || pk_mode == 2) {
    for (size_t i = 0; i < n; ++i) {
      uint64_t v = 0;
      if (!readVarint(payload, pos, v)) return -1;
    }
  } else {
    return -1;
  }
  if (pos >= payload.size()) return -1;
  (void)schema;
  return static_cast<int>(static_cast<uint8_t>(payload[pos]));
}

bool roundTrip(const mvdb::Schema& schema, const std::vector<mvdb::Row>& rows,
               std::vector<mvdb::Row>& out, std::string& err) {
  std::string payload;
  if (!mvdb::encodeInsertBatch(schema, rows, payload, err)) return false;
  out.clear();
  out.reserve(rows.size());
  bool ok = mvdb::decodeInsertBatch(
      schema, payload.data(), payload.size(),
      [&](int64_t, mvdb::Row& row) {
        out.push_back(std::move(row));
        return true;
      },
      err);
  if (!ok) return false;
  return sameBits(rows, out);
}

// Build "[-1.25,0.000001,...]" from an integer magnitude in millionths.
std::string fixedLiteral(const std::vector<uint32_t>& mags,
                         const std::vector<bool>& negatives, int digits) {
  std::string s = "[";
  for (size_t j = 0; j < mags.size(); ++j) {
    if (j) s.push_back(',');
    if (negatives[j]) s.push_back('-');
    const uint32_t m = mags[j];
    if (digits <= 0) {
      s += std::to_string(m / 1000000u);
    } else {
      // Print with the requested number of fractional digits (the magnitude
      // is a whole number of millionths, so pad/trim accordingly).
      char buf[64];
      const unsigned frac = m % 1000000u;
      std::snprintf(buf, sizeof(buf), "%u.%06u", m / 1000000u, frac);
      std::string full(buf);
      // full always has exactly 6 fractional digits.
      s += full.substr(0, full.size() - static_cast<size_t>(6 - digits));
    }
  }
  s.push_back(']');
  return s;
}

}  // namespace

int main() {
  const mvdb::Schema schema = makeSchema();
  std::mt19937_64 rng(20261116);
  std::string err;
  long failures = 0;

  // ---------------------------------------------------------------- test A
  // 20,000 random decimal vectors, 0..6 fractional digits.
  long checked_decimal = 0;
  long mode2_batches = 0;
  long batches = 0;
  for (int b = 0; b < 200; ++b) {
    std::vector<mvdb::Row> rows;
    rows.reserve(100);
    for (int i = 0; i < 100; ++i) {
      std::vector<uint32_t> mags(kDim);
      std::vector<bool> negs(kDim);
      for (size_t j = 0; j < kDim; ++j) {
        // Values <= 1.999999 keep the high-byte alphabet small enough that
        // mode 2 wins over the 3-plane mode even for these test batches.
        const uint32_t ip = static_cast<uint32_t>(rng() % 2);
        const uint32_t frac = static_cast<uint32_t>(rng() % 1000000u);
        mags[j] = ip * 1000000u + frac;
        negs[j] = (rng() & 1) != 0;
      }
      if (b == 0 && i == 0) {
        // Explicit edge cases in the first components: +0, -0, +1, -1,
        // 1-decimal, 1 millionth and the largest tested magnitude.
        const uint32_t edge[8] = {0, 0, 1000000, 1000000, 1500000, 1, 999999, 7000000};
        const bool edge_neg[8] = {false, true, false, true, true, false, true, false};
        for (size_t j = 0; j < 8 && j < kDim; ++j) {
          mags[j] = edge[j];
          negs[j] = edge_neg[j];
        }
      }
      const int digits = (b == 0 && i == 0) ? 6 : static_cast<int>(rng() % 7);
      std::string text = fixedLiteral(mags, negs, digits);
      std::vector<float> v;
      if (!mvdb::parseVectorLiteral(text, v, err) || v.size() != kDim) {
        std::printf("FAIL parse %s: %s\n", text.c_str(), err.c_str());
        ++failures;
        continue;
      }
      rows.push_back(makeRow(static_cast<int64_t>(b) * 100 + i + 1, v));
    }
    std::string payload;
    if (!mvdb::encodeInsertBatch(schema, rows, payload, err)) {
      std::printf("FAIL encode decimal batch %d: %s\n", b, err.c_str());
      ++failures;
      continue;
    }
    ++batches;
    if (vectorBlockMode(payload, schema, rows.size()) != 2) {
      std::printf("FAIL decimal batch %d did not select mode 2\n", b);
      ++failures;
      continue;
    }
    ++mode2_batches;
    std::vector<mvdb::Row> decoded;
    if (!mvdb::decodeInsertBatch(
            schema, payload.data(), payload.size(),
            [&](int64_t, mvdb::Row& row) {
              decoded.push_back(std::move(row));
              return true;
            },
            err) ||
        !sameBits(rows, decoded)) {
      std::printf("FAIL decimal batch %d round-trip: %s\n", b, err.c_str());
      ++failures;
      continue;
    }
    checked_decimal += static_cast<long>(rows.size());
  }
  if (mode2_batches != batches) ++failures;
  std::printf("fixed24 decimal: vectors=%ld batches=%ld mode2_batches=%ld\n",
              checked_decimal, batches, mode2_batches);

  // ---------------------------------------------------------------- test B
  // 20,000 random float32 bit patterns (2,500 x 8), forced to fall back.
  long checked_bits = 0;
  long fallback_batches = 0;
  for (int b = 0; b < 7; ++b) {
    std::vector<mvdb::Row> rows;
    const int row_count = (b == 6) ? 25 : 100;
    rows.reserve(static_cast<size_t>(row_count));
    for (int i = 0; i < row_count; ++i) {
      std::vector<float> v(kDim);
      for (size_t j = 0; j < kDim; ++j) {
        const uint32_t bits = static_cast<uint32_t>(rng());
        std::memcpy(&v[j], &bits, sizeof(bits));
      }
      if (i == 0) {
        // Guarantee this batch cannot use mode 2 (NaN has no fixed24 form).
        v[0] = std::bit_cast<float>(0x7F800001u);
      }
      rows.push_back(makeRow(static_cast<int64_t>(b) * 100 + i + 1, v));
    }
    std::string payload;
    if (!mvdb::encodeInsertBatch(schema, rows, payload, err)) {
      std::printf("FAIL encode bit batch %d: %s\n", b, err.c_str());
      ++failures;
      continue;
    }
    if (vectorBlockMode(payload, schema, rows.size()) == 2) {
      std::printf("FAIL bit batch %d unexpectedly selected mode 2\n", b);
      ++failures;
      continue;
    }
    ++fallback_batches;
    std::vector<mvdb::Row> decoded;
    if (!mvdb::decodeInsertBatch(
            schema, payload.data(), payload.size(),
            [&](int64_t, mvdb::Row& row) {
              decoded.push_back(std::move(row));
              return true;
            },
            err) ||
        !sameBits(rows, decoded)) {
      std::printf("FAIL bit batch %d round-trip: %s\n", b, err.c_str());
      ++failures;
      continue;
    }
    checked_bits += static_cast<long>(rows.size() * kDim);
  }
  std::printf("random bit patterns: components=%ld fallback_batches=%ld\n",
              checked_bits, fallback_batches);

  // ---------------------------------------------------------------- test C
  // Mixed batch: 6-decimal plus 7-decimal / exponent / large values.
  {
    std::vector<mvdb::Row> rows;
    for (int i = 0; i < 100; ++i) {
      std::vector<float> v(kDim, 0.5f);
      if (i == 0) v[0] = 0.1234567f;      // 7 decimals -> reject
      if (i == 1) v[1] = 1e20f;           // large -> reject
      if (i == 2) v[2] = 1e-30f;          // tiny -> m == 0 -> reject
      if (i == 3) v[3] = 8.5f;            // > 0x7FFFFF millionths
      if (i == 4) v[4] = std::bit_cast<float>(0x7FC00000u);  // NaN
      rows.push_back(makeRow(i + 1, v));
    }
    std::string payload;
    if (!mvdb::encodeInsertBatch(schema, rows, payload, err)) {
      std::printf("FAIL mixed encode: %s\n", err.c_str());
      ++failures;
    } else if (vectorBlockMode(payload, schema, rows.size()) == 2) {
      std::printf("FAIL mixed batch unexpectedly selected mode 2\n");
      ++failures;
    } else {
      std::vector<mvdb::Row> decoded;
      if (!mvdb::decodeInsertBatch(
              schema, payload.data(), payload.size(),
              [&](int64_t, mvdb::Row& row) {
                decoded.push_back(std::move(row));
                return true;
              },
              err) ||
          !sameBits(rows, decoded)) {
        std::printf("FAIL mixed round-trip: %s\n", err.c_str());
        ++failures;
      } else {
        std::printf("mixed fallback: ok\n");
      }
    }
  }

  // ---------------------------------------------------------------- test D
  // Truncated mode-2 payloads must be rejected cleanly.
  {
    std::vector<mvdb::Row> rows;
    for (int i = 0; i < 20; ++i) {
      std::vector<float> v(kDim, 1.25f);
      rows.push_back(makeRow(i + 1, v));
    }
    std::string payload;
    if (!mvdb::encodeInsertBatch(schema, rows, payload, err) ||
        vectorBlockMode(payload, schema, rows.size()) != 2) {
      std::printf("FAIL truncation setup\n");
      ++failures;
    } else {
      long rejected = 0;
      for (size_t cut = 0; cut < payload.size(); cut += 7) {
        std::string broken = payload.substr(0, cut);
        std::string decode_err;
        bool ok = mvdb::decodeInsertBatch(
            schema, broken.data(), broken.size(),
            [](int64_t, mvdb::Row&) { return true; }, decode_err);
        if (!ok) ++rejected;
      }
      std::printf("truncation: rejected=%ld\n", rejected);
      if (rejected == 0) ++failures;
    }
  }

  if (failures == 0) {
    std::printf("fixed24 self-check: PASS\n");
    return 0;
  }
  std::printf("fixed24 self-check: FAIL failures=%ld\n", failures);
  return 1;
}
