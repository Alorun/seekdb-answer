#include "executor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <immintrin.h>
#include <queue>
#include <set>
#include <thread>
#include <sstream>
#include <unordered_set>

namespace mvdb {

namespace {

struct OutCol {
  enum class Kind { Value, Distance } kind = Kind::Value;
  int col = -1;
  std::string header;
  std::vector<float> query;
};

struct KeyValue {
  enum class Kind { Int, Double, Str } kind = Kind::Int;
  int64_t i = 0;
  double d = 0;
  std::string str;
};

struct OrderSpec {
  bool is_distance = false;
  int col = -1;              // value column
  std::vector<float> query;  // distance query
  bool desc = false;
};

struct Candidate {
  int64_t pk = 0;
  std::vector<std::string> out;
  std::vector<KeyValue> keys;
};

std::string formatValue(const Value& v) {
  switch (v.type) {
    case Value::Type::Int:
      return std::to_string(v.i);
    case Value::Type::Str:
      return v.s;
    case Value::Type::Vec: {
      std::string out = "[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) out += ",";
        out += formatDouble(v.v[i]);
      }
      out += "]";
      return out;
    }
  }
  return {};
}

std::vector<std::string> formatProjection(const Row& row,
                                         const std::vector<OutCol>& cols) {
  std::vector<std::string> out;
  out.reserve(cols.size());
  for (const auto& col : cols) {
    if (col.kind == OutCol::Kind::Value) {
      out.push_back(formatValue(row.values[col.col]));
    } else {
      out.push_back(formatDouble(l2Distance(row.values[col.col].v, col.query)));
    }
  }
  return out;
}

bool resolveConditions(const Table& table, const std::vector<Condition>& where,
                       std::vector<ResolvedCond>& out, Status& err) {
  for (const auto& c : where) {
    int idx = findColumn(table.schema, c.column);
    if (idx < 0) {
      err = Status::Error(Err::kColumnNotFound, "unknown column: " + c.column);
      return false;
    }
    const Column& col = table.schema.columns[idx];
    if (col.type == ColumnType::Vector) {
      err = Status::Error(Err::kUnsupported, "comparison on VECTOR column is not supported");
      return false;
    }
    if (col.type == ColumnType::BigInt && c.value.type != Value::Type::Int) {
      err = Status::Error(Err::kInvalidValue, "BIGINT column requires integer literal: " + c.column);
      return false;
    }
    if (col.type == ColumnType::Varchar && c.value.type != Value::Type::Str) {
      err = Status::Error(Err::kInvalidValue, "VARCHAR column requires string literal: " + c.column);
      return false;
    }
    ResolvedCond rc;
    rc.col = idx;
    rc.op = c.op;
    rc.literal = c.value;
    out.push_back(std::move(rc));
  }
  return true;
}

bool matchConditions(const Row& row, const std::vector<ResolvedCond>& conds,
                     Status& err) {
  for (const auto& c : conds) {
    const Value& v = row.values[c.col];
    int cmp = 0;
    if (v.type == Value::Type::Int) {
      if (c.literal.type != Value::Type::Int) {
        err = Status::Error(Err::kInvalidValue, "literal type mismatch");
        return false;
      }
      cmp = (v.i < c.literal.i) ? -1 : (v.i > c.literal.i ? 1 : 0);
    } else if (v.type == Value::Type::Str) {
      if (c.literal.type != Value::Type::Str) {
        err = Status::Error(Err::kInvalidValue, "literal type mismatch");
        return false;
      }
      cmp = v.s.compare(c.literal.s);
    } else {
      err = Status::Error(Err::kUnsupported, "comparison on VECTOR column is not supported");
      return false;
    }
    bool ok = false;
    if (c.op == "=") ok = (cmp == 0);
    else if (c.op == "!=") ok = (cmp != 0);
    else if (c.op == "<") ok = (cmp < 0);
    else if (c.op == "<=") ok = (cmp <= 0);
    else if (c.op == ">") ok = (cmp > 0);
    else if (c.op == ">=") ok = (cmp >= 0);
    if (!ok) return false;
  }
  return true;
}

}  // namespace

int findColumn(const Schema& schema, const std::string& name) {
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (iequals(schema.columns[i].name, name)) return static_cast<int>(i);
  }
  return -1;
}

namespace {

double l2ScalarSq(const float* a, const float* b, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) double l2Avx2DoubleSq(const float* a,
                                                          const float* b,
                                                          size_t n) {
  // Accumulate in double: the inputs are float32 but a finite component can be
  // as large as FLT_MAX; float accumulation would overflow the squared distance
  // and can also lose precision for large-but-finite vectors.
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    // Widen before subtracting: the difference of two finite float32 values
    // can exceed FLT_MAX and would become inf in a float vsubps.
    __m256d a0 = _mm256_cvtps_pd(_mm256_castps256_ps128(va));
    __m256d a1 = _mm256_cvtps_pd(_mm256_extractf128_ps(va, 1));
    __m256d b0 = _mm256_cvtps_pd(_mm256_castps256_ps128(vb));
    __m256d b1 = _mm256_cvtps_pd(_mm256_extractf128_ps(vb, 1));
    __m256d d0 = _mm256_sub_pd(a0, b0);
    __m256d d1 = _mm256_sub_pd(a1, b1);
    acc0 = _mm256_fmadd_pd(d0, d0, acc0);
    acc1 = _mm256_fmadd_pd(d1, d1, acc1);
  }
  alignas(32) double tmp0[4];
  alignas(32) double tmp1[4];
  _mm256_store_pd(tmp0, acc0);
  _mm256_store_pd(tmp1, acc1);
  double sum = tmp0[0] + tmp0[1] + tmp0[2] + tmp0[3] +
               tmp1[0] + tmp1[1] + tmp1[2] + tmp1[3];
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) double l2Avx2FastSq(const float* a,
                                                          const float* b,
                                                          size_t n) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    acc = _mm256_fmadd_ps(d, d, acc);
  }
  alignas(32) float tmp[8];
  _mm256_store_ps(tmp, acc);
  float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] +
              tmp[6] + tmp[7];
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return static_cast<double>(sum);
}

__attribute__((target("avx2,fma"))) double l2Avx2Sq(const float* a,
                                                    const float* b, size_t n) {
  double fast = l2Avx2FastSq(a, b, n);
  if (std::isfinite(fast)) return fast;
  // Valid float32 components can overflow the float accumulator or their
  // difference; recompute in double only on that rare path.
  return l2Avx2DoubleSq(a, b, n);
}

__attribute__((target("avx2,fma"))) double l2DotDistanceSq(
    const float* a, const float* b, size_t n, double na, double nb) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    acc = _mm256_fmadd_ps(va, vb, acc);
  }
  alignas(32) float tmp[8];
  _mm256_store_ps(tmp, acc);
  float dot = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] +
              tmp[6] + tmp[7];
  for (; i < n; ++i) dot += a[i] * b[i];
  if (!std::isfinite(dot)) return std::numeric_limits<double>::quiet_NaN();
  double d = na + nb - 2.0 * static_cast<double>(dot);
  return d < 0.0 ? 0.0 : d;
}

__attribute__((target("avx2"))) int quantDistanceSq(const uint8_t* a,
                                                     const uint8_t* b, size_t n) {
  __m256i acc = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(va, vb));
  }
  alignas(32) uint64_t tmp[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(tmp), acc);
  int sum = 0;
  for (uint64_t x : tmp) sum += static_cast<int>(x);
  for (; i < n; ++i) {
    int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
    sum += d < 0 ? -d : d;
  }
  return sum;
}

using L2SqFn = double (*)(const float*, const float*, size_t);
L2SqFn pickL2Sq() {
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    return l2Avx2Sq;
  }
  return l2ScalarSq;
}

}  // namespace

double l2DistanceSq(const float* a, const float* b, size_t n) {
  static const L2SqFn fn = pickL2Sq();
  return fn(a, b, n);
}

double l2DistanceSq(const std::vector<float>& a, const std::vector<float>& b) {
  return l2DistanceSq(a.data(), b.data(), std::min(a.size(), b.size()));
}

double l2Distance(const std::vector<float>& a, const std::vector<float>& b) {
  return std::sqrt(l2DistanceSq(a, b));
}

namespace {

void rebuildVectorCache(Table& table, int vec_col) {
  VectorCache& c = table.vector_cache;
  c.valid = false;
  c.pks.clear();
  c.rows.clear();
  c.vec.clear();
  c.vec_col = vec_col;
  c.dim = table.schema.columns[vec_col].length;
  const size_t n = table.rows.size();
  c.pks.reserve(n);
  c.rows.reserve(n);
  c.vec.reserve(n * c.dim);
  for (const auto& [pk, row] : table.rows) {
    const std::vector<float>& v = row.values[vec_col].v;
    if (v.size() != c.dim) return;
    c.pks.push_back(pk);
    c.rows.push_back(&row);
    c.vec.insert(c.vec.end(), v.begin(), v.end());
  }
  c.version = table.row_version;
  c.valid = true;
}

void ensureVectorCache(Table& table, int vec_col) {
  VectorCache& c = table.vector_cache;
  if (c.valid && c.version == table.row_version && c.vec_col == vec_col &&
      c.pks.size() == table.rows.size()) {
    return;
  }
  rebuildVectorCache(table, vec_col);
}

void rebuildFilterCache(Table& table, int col) {
  FilterCache c;
  c.version = table.row_version;
  c.col = col;
  c.valid = true;
  const bool is_str = table.schema.columns[col].type == ColumnType::Varchar;
  for (const auto& [pk, row] : table.rows) {
    const Value& v = row.values[col];
    if (is_str) {
      if (v.type == Value::Type::Str) c.str[v.s].push_back(pk);
    } else if (v.type == Value::Type::Int) {
      c.num[v.i].push_back(pk);
    }
  }
  table.filter_caches[col] = std::move(c);
}

FilterCache* ensureFilterCache(Table& table, int col) {
  auto it = table.filter_caches.find(col);
  if (it != table.filter_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  rebuildFilterCache(table, col);
  return &table.filter_caches[col];
}

void buildIvf(Table& table, int col) {
  const size_t n = table.rows.size();
  if (n < 2000) return;
  IvfCache c;
  c.col = col;
  c.dim = table.schema.columns[col].length;
  c.nlist = std::max<size_t>(8, std::min<size_t>(256, static_cast<size_t>(std::sqrt(static_cast<double>(n)))));
  c.nprobe = std::min<size_t>(c.nlist, 16);
  ensureVectorCache(table, col);
  const VectorCache& vc = table.vector_cache;
  if (!vc.valid || vc.dim != c.dim) return;
  c.centroids.assign(c.nlist, std::vector<float>(c.dim, 0.0f));
  // Deterministic, spread-out initialization from the PK-ordered rows.
  size_t k = 0;
  for (size_t i = 0; i < vc.pks.size() && k < c.nlist; ++i) {
    std::copy(vc.vec.begin() + i * c.dim,
              vc.vec.begin() + (i + 1) * c.dim, c.centroids[k].begin());
    ++k;
  }
  if (k < c.nlist) return;
  std::vector<size_t> assign(n, 0);
  std::vector<size_t> counts(c.nlist, 0);
  std::vector<std::vector<float>> sums(c.nlist, std::vector<float>(c.dim, 0.0f));
  for (int iter = 0; iter < 5; ++iter) {
    for (auto& s : sums) std::fill(s.begin(), s.end(), 0.0f);
    std::fill(counts.begin(), counts.end(), 0);
    for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
      const float* v = vc.vec.data() + idx * c.dim;
      size_t best = 0;
      double bestd = std::numeric_limits<double>::infinity();
      for (size_t q = 0; q < c.nlist; ++q) {
        double d = l2DistanceSq(v, c.centroids[q].data(), c.dim);
        if (d < bestd) { bestd = d; best = q; }
      }
      assign[idx] = best;
      ++counts[best];
      for (size_t j = 0; j < c.dim; ++j) sums[best][j] += v[j];
    }
    for (size_t q = 0; q < c.nlist; ++q) {
      if (counts[q] == 0) continue;
      for (size_t j = 0; j < c.dim; ++j) {
        c.centroids[q][j] = sums[q][j] / static_cast<float>(counts[q]);
      }
    }
  }
  c.lists.assign(c.nlist, {});
  for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
    c.lists[assign[idx]].push_back(idx);
  }
  c.version = table.row_version;
  c.valid = true;
  table.ivf_caches[col] = std::move(c);
}

IvfCache* ensureIvf(Table& table, int col) {
  auto it = table.ivf_caches.find(col);
  if (it != table.ivf_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  buildIvf(table, col);
  it = table.ivf_caches.find(col);
  return it == table.ivf_caches.end() ? nullptr : &it->second;
}

void buildQuant(Table& table, int col) {
  ensureVectorCache(table, col);
  const VectorCache& vc = table.vector_cache;
  if (!vc.valid || vc.pks.empty()) return;
  QuantCache c;
  c.col = col;
  c.dim = vc.dim;
  c.version = table.row_version;
  float lo = std::numeric_limits<float>::max();
  float hi = -std::numeric_limits<float>::max();
  for (float v : vc.vec) {
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  if (lo == std::numeric_limits<float>::max() || hi <= lo) return;
  c.minv = lo;
  c.scale = 255.0f / (hi - lo);
  c.q.resize(vc.vec.size());
  for (size_t i = 0; i < vc.vec.size(); ++i) {
    int q = static_cast<int>(std::lround((vc.vec[i] - lo) * c.scale));
    c.q[i] = static_cast<uint8_t>(std::max(0, std::min(255, q)));
  }
  c.valid = true;
  table.quant_caches[col] = std::move(c);
}

QuantCache* ensureQuant(Table& table, int col) {
  auto it = table.quant_caches.find(col);
  if (it != table.quant_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  buildQuant(table, col);
  it = table.quant_caches.find(col);
  return it == table.quant_caches.end() ? nullptr : &it->second;
}

}  // namespace

Status convertValue(const Column& col, const Value& input, Value& out) {
  switch (col.type) {
    case ColumnType::BigInt:
      if (input.type != Value::Type::Int) {
        return Status::Error(Err::kInvalidValue, "expected BIGINT value for column " + col.name);
      }
      out = input;
      return Status::Ok();
    case ColumnType::Varchar:
      if (input.type != Value::Type::Str) {
        return Status::Error(Err::kInvalidValue, "expected VARCHAR value for column " + col.name);
      }
      if (utf8Length(input.s) > col.length) {
        return Status::Error(Err::kDataTooLong,
                             "value too long for VARCHAR(" + std::to_string(col.length) +
                                 ") column " + col.name);
      }
      out = input;
      return Status::Ok();
    case ColumnType::Vector: {
      if (input.type != Value::Type::Str) {
        return Status::Error(Err::kInvalidValue,
                             "expected vector literal string for column " + col.name);
      }
      std::vector<float> v;
      std::string err;
      if (!parseVectorLiteral(input.s, v, err)) {
        return Status::Error(Err::kInvalidValue, err);
      }
      if (v.size() != col.length) {
        return Status::Error(Err::kInvalidValue,
                             "vector dimension mismatch for column " + col.name +
                                 ": expected " + std::to_string(col.length) + ", got " +
                                 std::to_string(v.size()));
      }
      out = Value::MakeVec(std::move(v));
      return Status::Ok();
    }
  }
  return Status::Error(Err::kInvalidValue, "unknown column type");
}

bool rowMatches(const Table& table, const Row& row,
                const std::vector<Condition>& where, std::string& err) {
  std::vector<ResolvedCond> conds;
  Status st;
  if (!resolveConditions(table, where, conds, st)) {
    err = st.message;
    return false;
  }
  Status m;
  if (!matchConditions(row, conds, m)) {
    if (!m.ok()) err = m.message;
    return false;
  }
  return true;
}

Status Executor::beginTxn() {
  if (active_) {
    return Status::Error(Err::kTxnState, "transaction already active");
  }
  active_ = true;
  txn_id_ = db_.newTxnId();
  txn_start_ = db_.wal().size();
  first_lsn_ = 0;
  last_lsn_ = 0;
  op_count_ = 0;
  undo_.clear();
  return Status::Ok();
}

Status Executor::restoreTo(size_t undo_size, uint64_t wal_offset) {
  while (undo_.size() > undo_size) {
    Undo u = std::move(undo_.back());
    undo_.pop_back();
    Table* table = db_.tableById(u.table_id);
    if (!table) continue;
    if (!table->disk_mode) {
      if (u.had_row) {
        table->rows[u.pk] = std::move(u.old_row);
      } else {
        table->rows.erase(u.pk);
      }
    }
    if (u.had_location) {
      table->locations[u.pk] = u.old_location;
    } else {
      table->locations.erase(u.pk);
    }
    ++table->row_version;
  }
  return db_.wal().truncate(wal_offset);
}

Status Executor::rollbackTxn() {
  if (!active_) return Status::Ok();
  Status st = restoreTo(0, txn_start_);
  active_ = false;
  undo_.clear();
  op_count_ = 0;
  return st;
}

Status Executor::writeCommit() {
  if (op_count_ == 0) return Status::Ok();
  uint64_t lsn = db_.newLsn();
  Status st = db_.wal().appendCommit(txn_id_, op_count_, first_lsn_, last_lsn_, lsn);
  if (!st.ok()) return st;
  return db_.wal().sync();
}

Status Executor::commitTxn() {
  if (!active_) return Status::Ok();
  Status st = writeCommit();
  if (!st.ok()) {
    Status rb = restoreTo(0, txn_start_);
    active_ = false;
    undo_.clear();
    op_count_ = 0;
    if (!rb.ok()) return rb;
    return st;
  }
  maybeCrash("after_commit_fsync");
  active_ = false;
  undo_.clear();
  op_count_ = 0;
  return Status::Ok();
}

Status Executor::shutdown() {
  if (active_) return rollbackTxn();
  return Status::Ok();
}

Status Executor::writeRowRecord(WalType type, const Table& table, int64_t pk,
                                const Row& row, uint64_t* out_offset) {
  WalRecord rec;
  rec.type = static_cast<uint8_t>(type);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table.schema.table_id;
  rec.pk = static_cast<uint64_t>(pk);
  if (type != WalType::Delete) {
    rec.payload = encodeRow(table.schema, row);
  }
  Status st = db_.wal().append(rec, out_offset);
  if (!st.ok()) return st;
  maybeCrash("after_wal_write");
  if (first_lsn_ == 0) first_lsn_ = rec.lsn;
  last_lsn_ = rec.lsn;
  ++op_count_;
  return Status::Ok();
}

Status Executor::readRowAt(const Table& table, uint64_t offset, Row& row,
                           std::string& err) const {
  WalRecord rec;
  uint64_t next = 0;
  auto r = db_.wal().readRecord(offset, rec, next, err);
  if (r != Wal::ReadResult::Ok) {
    if (err.empty()) err = "record not found";
    return Status::Error(Err::kIo, "read row failed: " + err);
  }
  if (rec.type != static_cast<uint8_t>(WalType::Insert) &&
      rec.type != static_cast<uint8_t>(WalType::Update)) {
    err = "location does not point to a row";
    return Status::Error(Err::kIo, err);
  }
  if (!decodeRow(table.schema, rec.payload, row, err)) {
    return Status::Error(Err::kIo, "decode row failed: " + err);
  }
  return Status::Ok();
}

Status Executor::findRow(Table& table, int64_t pk, Row& row, bool& found) {
  found = false;
  if (table.disk_mode) {
    auto it = table.locations.find(pk);
    if (it == table.locations.end()) return Status::Ok();
    std::string err;
    Status st = readRowAt(table, it->second, row, err);
    if (!st.ok()) return st;
    found = true;
    return Status::Ok();
  }
  auto it = table.rows.find(pk);
  if (it == table.rows.end()) return Status::Ok();
  row = it->second;
  found = true;
  return Status::Ok();
}

void Executor::spillIfNeeded(Table& table) {
  if (!table.disk_mode && table.rows.size() > db_.memoryRows()) {
    table.rows.clear();
    table.disk_mode = true;
  }
}

Status Executor::applyInsert(Table& table, int64_t pk, Row row) {
  if (table.locations.count(pk)) {
    return Status::Error(Err::kDuplicateKey,
                         "duplicate primary key: " + std::to_string(pk));
  }
  uint64_t offset = 0;
  Status st = writeRowRecord(WalType::Insert, table, pk, row, &offset);
  if (!st.ok()) return st;
  table.locations[pk] = offset;
  if (!table.disk_mode) {
    table.rows[pk] = row;
  }
  spillIfNeeded(table);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = false;
  u.had_location = false;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::applyUpdate(Table& table, int64_t pk, Row row) {
  Row old;
  bool found = false;
  Status f = findRow(table, pk, old, found);
  if (!f.ok()) return f;
  if (!found) return Status::Error(Err::kDuplicateKey, "row not found: " + std::to_string(pk));
  auto loc_it = table.locations.find(pk);
  if (loc_it == table.locations.end()) {
    return Status::Error(Err::kIo, "row has no location: " + std::to_string(pk));
  }
  const uint64_t old_location = loc_it->second;
  const bool had_row = !table.disk_mode;
  uint64_t offset = 0;
  Status st = writeRowRecord(WalType::Update, table, pk, row, &offset);
  if (!st.ok()) return st;
  table.locations[pk] = offset;
  if (!table.disk_mode) {
    table.rows[pk] = row;
  }
  spillIfNeeded(table);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  if (had_row) u.old_row = std::move(old);
  u.had_location = true;
  u.old_location = old_location;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::applyDelete(Table& table, int64_t pk) {
  Row old;
  bool found = false;
  Status f = findRow(table, pk, old, found);
  if (!f.ok()) return f;
  if (!found) return Status::Error(Err::kDuplicateKey, "row not found: " + std::to_string(pk));
  auto loc_it = table.locations.find(pk);
  if (loc_it == table.locations.end()) {
    return Status::Error(Err::kIo, "row has no location: " + std::to_string(pk));
  }
  const uint64_t old_location = loc_it->second;
  const bool had_row = !table.disk_mode;
  Row empty;
  Status st = writeRowRecord(WalType::Delete, table, pk, empty);
  if (!st.ok()) return st;
  table.locations.erase(pk);
  if (!table.disk_mode) table.rows.erase(pk);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  if (had_row) u.old_row = std::move(old);
  u.had_location = true;
  u.old_location = old_location;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::collectMatchingPks(Table& table,
                                    const std::vector<ResolvedCond>& conds,
                                    std::vector<int64_t>& pks) {
  if (!table.disk_mode) {
    for (const auto& [pk, row] : table.rows) {
      Status m;
      if (matchConditions(row, conds, m)) {
        pks.push_back(pk);
      } else if (!m.ok()) {
        return m;
      }
    }
    return Status::Ok();
  }
  Status row_status = Status::Ok();
  Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
    if (v.type != static_cast<uint8_t>(WalType::Insert) &&
        v.type != static_cast<uint8_t>(WalType::Update)) {
      return true;
    }
    if (v.table != table.schema.table_id) return true;
    auto it = table.locations.find(static_cast<int64_t>(v.pk));
    if (it == table.locations.end() || it->second != v.offset) return true;
    Row row;
    std::string err;
    if (!decodeRow(table.schema, v.payload, v.payload_size, row, err)) {
      row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
      return false;
    }
    Status m;
    if (matchConditions(row, conds, m)) {
      pks.push_back(static_cast<int64_t>(v.pk));
    } else if (!m.ok()) {
      row_status = m;
      return false;
    }
    return true;
  }, nullptr, false);
  if (!scan.ok()) return scan;
  return row_status;
}

Status Executor::executeInsert(const InsertStmt& stmt) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const Schema& schema = table->schema;
  std::vector<int> col_map;
  if (stmt.columns.empty()) {
    col_map.resize(schema.columns.size());
    for (size_t i = 0; i < schema.columns.size(); ++i) col_map[i] = static_cast<int>(i);
  } else {
    col_map.resize(stmt.columns.size(), -1);
    std::vector<bool> seen(schema.columns.size(), false);
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
      int idx = findColumn(schema, stmt.columns[i]);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column: " + stmt.columns[i]);
      }
      if (seen[idx]) {
        return Status::Error(Err::kColumnCount, "duplicate column: " + stmt.columns[i]);
      }
      seen[idx] = true;
      col_map[i] = idx;
    }
    for (size_t i = 0; i < seen.size(); ++i) {
      if (!seen[i]) {
        return Status::Error(Err::kColumnCount,
                             "INSERT must provide all columns in this build (missing " +
                                 schema.columns[i].name + ")");
      }
    }
  }

  for (const auto& raw : stmt.rows) {
    if (raw.size() != col_map.size()) {
      return Status::Error(Err::kColumnCount,
                           "column count mismatch: expected " +
                               std::to_string(col_map.size()) + ", got " +
                               std::to_string(raw.size()));
    }
    Row row;
    row.values.resize(schema.columns.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      const Column& col = schema.columns[col_map[i]];
      Value converted;
      Status st = convertValue(col, raw[i], converted);
      if (!st.ok()) return st;
      row.values[col_map[i]] = std::move(converted);
    }
    int64_t pk = row.values[schema.pk_index].i;
    Status st = applyInsert(*table, pk, std::move(row));
    if (!st.ok()) return st;
  }
  return Status::Ok();
}

Status Executor::executeUpdate(const UpdateStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  struct Assign {
    int col;
    Value value;
  };
  std::vector<Assign> assigns;
  std::unordered_set<int> assigned;
  for (const auto& [name, lit] : stmt.sets) {
    int idx = findColumn(table->schema, name);
    if (idx < 0) {
      return Status::Error(Err::kColumnNotFound, "unknown column: " + name);
    }
    if (idx == table->schema.pk_index) {
      return Status::Error(Err::kUnsupported, "updating the primary key is not supported");
    }
    if (assigned.count(idx)) {
      return Status::Error(Err::kSyntax, "column assigned twice: " + name);
    }
    assigned.insert(idx);
    Value converted;
    Status st = convertValue(table->schema.columns[idx], lit, converted);
    if (!st.ok()) return st;
    assigns.push_back({idx, std::move(converted)});
  }
  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<int64_t> pks;
  Status collect = collectMatchingPks(*table, conds, pks);
  if (!collect.ok()) return collect;
  for (int64_t pk : pks) {
    Row old;
    bool found = false;
    Status f = findRow(*table, pk, old, found);
    if (!f.ok()) return f;
    if (!found) continue;
    Row new_row = std::move(old);
    for (const auto& a : assigns) new_row.values[a.col] = a.value;
    Status s = applyUpdate(*table, pk, std::move(new_row));
    if (!s.ok()) return s;
  }
  out.affected = pks.size();
  return Status::Ok();
}

Status Executor::executeDelete(const DeleteStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<int64_t> pks;
  Status collect = collectMatchingPks(*table, conds, pks);
  if (!collect.ok()) return collect;
  for (int64_t pk : pks) {
    Status s = applyDelete(*table, pk);
    if (!s.ok()) return s;
  }
  out.affected = pks.size();
  return Status::Ok();
}

Status Executor::executeCreateTable(const CreateTableStmt& stmt) {
  if (db_.findTable(stmt.table)) {
    if (stmt.if_not_exists) return Status::Ok();
    return Status::Error(Err::kTableExists, "table already exists: " + stmt.table);
  }
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Schema schema;
  schema.name = stmt.table;
  schema.columns = stmt.columns;
  schema.pk_index = stmt.pk_index;
  schema.vector_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].type == ColumnType::Vector) {
      if (schema.vector_index >= 0) {
        return Status::Error(Err::kUnsupported, "at most one VECTOR column per table");
      }
      schema.vector_index = static_cast<int>(i);
    }
  }
  schema.table_id = db_.newTableId();

  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::CreateTable);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = schema.table_id;
  rec.payload = encodeSchema(schema);
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;

  auto table = std::make_unique<Table>();
  table->schema = std::move(schema);
  return db_.addTable(std::move(table));
}

Status Executor::executeDropTable(const DropTableStmt& stmt) {
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    if (stmt.if_exists) return Status::Ok();
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const uint64_t table_id = table->schema.table_id;
  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::DropTable);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table_id;
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;
  return db_.dropTable(table_id);
}

Status Executor::executeTruncateTable(const std::string&) {
  return Status::Error(Err::kUnsupported,
                       "TRUNCATE is not supported by this task's SQL subset");
}

Status Executor::executeCreateIndex(const CreateIndexStmt& stmt) {
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  if (db_.indexExists(stmt.table, stmt.name)) {
    return Status::Error(Err::kIndexExists, "index already exists: " + stmt.name);
  }
  int idx = findColumn(table->schema, stmt.column);
  if (idx < 0) {
    return Status::Error(Err::kColumnNotFound, "unknown column: " + stmt.column);
  }
  if (table->schema.columns[idx].type != ColumnType::Vector) {
    return Status::Error(Err::kInvalidValue, "index column must be VECTOR");
  }
  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::CreateIndex);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table->schema.table_id;
  rec.payload = encodeIndex(stmt.name, static_cast<uint32_t>(idx));
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;
  return db_.createIndex(stmt.name, stmt.table, stmt.column);
}

Status Executor::executeSelect(const SelectStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const Schema& schema = table->schema;
  std::vector<OutCol> cols;
  for (const auto& item : stmt.items) {
    if (item.kind == SelectItem::Kind::Star) {
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        OutCol c;
        c.kind = OutCol::Kind::Value;
        c.col = static_cast<int>(i);
        c.header = schema.columns[i].name;
        cols.push_back(std::move(c));
      }
      continue;
    }
    if (item.kind == SelectItem::Kind::Column) {
      int idx = findColumn(schema, item.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
      }
      OutCol c;
      c.kind = OutCol::Kind::Value;
      c.col = idx;
      c.header = item.alias.empty() ? schema.columns[idx].name : item.alias;
      cols.push_back(std::move(c));
      continue;
    }
    int idx = findColumn(schema, item.column);
    if (idx < 0) {
      return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
    }
    if (schema.columns[idx].type != ColumnType::Vector) {
      return Status::Error(Err::kInvalidValue, "L2_DISTANCE requires a VECTOR column");
    }
    std::vector<float> query;
    std::string err;
    if (!parseVectorLiteral(item.vector_text, query, err)) {
      return Status::Error(Err::kInvalidValue, err);
    }
    if (query.size() != schema.columns[idx].length) {
      return Status::Error(Err::kInvalidValue,
                           "vector dimension mismatch in L2_DISTANCE: expected " +
                               std::to_string(schema.columns[idx].length) + ", got " +
                               std::to_string(query.size()));
    }
    OutCol c;
    c.kind = OutCol::Kind::Distance;
    c.col = idx;
    c.header = item.alias.empty() ? item.text : item.alias;
    c.query = std::move(query);
    cols.push_back(std::move(c));
  }
  if (cols.empty()) return Status::Error(Err::kSyntax, "empty projection");

  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<OrderSpec> orders;
  for (const auto& o : stmt.order) {
    OrderSpec spec;
    spec.desc = o.desc;
    if (o.ordinal > 0) {
      if (static_cast<size_t>(o.ordinal) > cols.size()) {
        return Status::Error(Err::kColumnNotFound,
                             "ORDER BY ordinal out of range");
      }
      const OutCol& oc = cols[static_cast<size_t>(o.ordinal - 1)];
      spec.is_distance = (oc.kind == OutCol::Kind::Distance);
      spec.col = oc.col;
      spec.query = oc.query;
      if (!spec.is_distance && schema.columns[spec.col].type == ColumnType::Vector) {
        return Status::Error(Err::kUnsupported,
                             "ORDER BY on VECTOR column is not supported");
      }
      orders.push_back(std::move(spec));
      continue;
    }
    if (o.is_distance) {
      int idx = findColumn(schema, o.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound,
                             "unknown column in ORDER BY: " + o.column);
      }
      if (schema.columns[idx].type != ColumnType::Vector) {
        return Status::Error(Err::kInvalidValue,
                             "L2_DISTANCE requires a VECTOR column: " + o.column);
      }
      std::vector<float> query;
      std::string err;
      if (!parseVectorLiteral(o.vector_text, query, err)) {
        return Status::Error(Err::kInvalidValue, err);
      }
      if (query.size() != schema.columns[idx].length) {
        return Status::Error(Err::kInvalidValue,
                             "vector dimension mismatch in ORDER BY");
      }
      spec.is_distance = true;
      spec.col = idx;
      spec.query = std::move(query);
      orders.push_back(std::move(spec));
      continue;
    }
    bool resolved = false;
    for (const auto& item : stmt.items) {
      if (item.alias.empty() || !iequals(item.alias, o.column)) continue;
      if (item.kind == SelectItem::Kind::Distance) {
        int idx = findColumn(schema, item.column);
        if (idx < 0) return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
        std::vector<float> query;
        std::string err;
        if (!parseVectorLiteral(item.vector_text, query, err)) {
          return Status::Error(Err::kInvalidValue, err);
        }
        if (query.size() != schema.columns[idx].length) {
          return Status::Error(Err::kInvalidValue, "vector dimension mismatch in ORDER BY");
        }
        spec.is_distance = true;
        spec.col = idx;
        spec.query = std::move(query);
        resolved = true;
      } else if (item.kind == SelectItem::Kind::Column) {
        int idx = findColumn(schema, item.column);
        if (idx < 0) return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
        spec.is_distance = false;
        spec.col = idx;
        resolved = true;
      }
      break;
    }
    if (!resolved) {
      int idx = findColumn(schema, o.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column in ORDER BY: " + o.column);
      }
      spec.is_distance = false;
      spec.col = idx;
    }
    if (!spec.is_distance && schema.columns[spec.col].type == ColumnType::Vector) {
      return Status::Error(Err::kUnsupported, "ORDER BY on VECTOR column is not supported");
    }
    orders.push_back(std::move(spec));
  }

  auto fill_result_columns = [&](ExecResult& r) {
    r.columns.clear();
    r.column_kinds.clear();
    r.columns.reserve(cols.size());
    r.column_kinds.reserve(cols.size());
    for (const auto& col : cols) {
      r.columns.push_back(col.header);
      ResultColumn rc;
      if (col.kind == OutCol::Kind::Distance) {
        rc.kind = ResultKind::Double;
      } else {
        const Column& schema_col = schema.columns[col.col];
        if (schema_col.type == ColumnType::BigInt) rc.kind = ResultKind::Int;
        else if (schema_col.type == ColumnType::Varchar) rc.kind = ResultKind::Str;
        else { rc.kind = ResultKind::Vec; rc.dim = schema_col.length; }
      }
      r.column_kinds.push_back(rc);
    }
  };
  const bool limit_zero = stmt.has_limit && stmt.limit == 0;
  const bool use_avx2 = __builtin_cpu_supports("avx2") &&
                        __builtin_cpu_supports("fma");
  const bool pk_order_topk = !limit_zero && stmt.has_limit &&
                             !table->disk_mode && orders.size() == 1 &&
                             !orders[0].is_distance &&
                             orders[0].col == schema.pk_index;
  if (pk_order_topk) {
    // The in-memory row map is already ordered by primary key. This avoids
    // building and sorting a Candidate (and its projection) for every row.
    out.has_result = true;
    fill_result_columns(out);
    out.rows.reserve(static_cast<size_t>(stmt.limit));
    auto take_row = [&](const Row& row) -> Status {
      Status m;
      if (!matchConditions(row, conds, m)) return m;
      out.rows.push_back(formatProjection(row, cols));
      return Status::Ok();
    };
    if (!orders[0].desc) {
      for (auto it = table->rows.begin(); it != table->rows.end(); ++it) {
        Status st2 = take_row(it->second);
        if (!st2.ok()) return st2;
        if (out.rows.size() >= stmt.limit) break;
      }
    } else {
      for (auto it = table->rows.rbegin(); it != table->rows.rend(); ++it) {
        Status st2 = take_row(it->second);
        if (!st2.ok()) return st2;
        if (out.rows.size() >= stmt.limit) break;
      }
    }
    return Status::Ok();
  }
  const bool distance_topk = !limit_zero && stmt.has_limit && orders.size() == 1 &&
                             orders[0].is_distance && !orders[0].desc;
  // ANN remains exact fallback: the tested IVF/quantized candidates did not
  // beat the optimized exact scan with enough margin on uniform data.
  const bool ann_quant = false;
  const bool order_topk = !limit_zero && stmt.has_limit && !orders.empty() &&
                          !distance_topk;
  const bool early_limit = !limit_zero && stmt.has_limit && orders.empty();
  const size_t k = (distance_topk || order_topk || early_limit)
                       ? static_cast<size_t>(stmt.limit)
                       : 0;

  struct Hit {
    double d = 0;
    int64_t pk = 0;
    Row row;
  };
  auto worse = [](const Hit& a, const Hit& b) {
    if (a.d != b.d) return a.d < b.d;
    return a.pk < b.pk;
  };
  std::priority_queue<Hit, std::vector<Hit>, decltype(worse)> heap(worse);

  auto candidate_better = [&](const Candidate& a, const Candidate& b) {
    for (size_t i = 0; i < orders.size(); ++i) {
      const KeyValue& x = a.keys[i];
      const KeyValue& y = b.keys[i];
      int cmp = 0;
      if (x.kind == KeyValue::Kind::Int && y.kind == KeyValue::Kind::Int) {
        cmp = (x.i < y.i) ? -1 : (x.i > y.i ? 1 : 0);
      } else if (x.kind == KeyValue::Kind::Double &&
                 y.kind == KeyValue::Kind::Double) {
        cmp = (x.d < y.d) ? -1 : (x.d > y.d ? 1 : 0);
      } else if (x.kind == KeyValue::Kind::Str &&
                 y.kind == KeyValue::Kind::Str) {
        cmp = x.str.compare(y.str);
      } else {
        long double lx = x.kind == KeyValue::Kind::Int ? x.i : x.d;
        long double ly = y.kind == KeyValue::Kind::Int ? y.i : y.d;
        cmp = (lx < ly) ? -1 : (lx > ly ? 1 : 0);
      }
      if (cmp != 0) return orders[i].desc ? (cmp > 0) : (cmp < 0);
    }
    return a.pk < b.pk;
  };
  std::priority_queue<Candidate, std::vector<Candidate>,
                      decltype(candidate_better)>
      candidate_heap(candidate_better);
  std::vector<Candidate> candidates;
  bool stop_scan = false;

  double worst_d = std::numeric_limits<double>::infinity();
  int64_t worst_pk = std::numeric_limits<int64_t>::max();
  auto consider_hit = [&](int64_t pk, const Row& row, double d) {
    if (heap.size() < k) {
      Hit h;
      h.d = d;
      h.pk = pk;
      h.row = row;
      heap.push(std::move(h));
      if (heap.size() == k) {
        worst_d = heap.top().d;
        worst_pk = heap.top().pk;
      }
    } else if (d < worst_d || (d == worst_d && pk < worst_pk)) {
      Hit h;
      h.d = d;
      h.pk = pk;
      h.row = row;
      heap.pop();
      heap.push(std::move(h));
      worst_d = heap.top().d;
      worst_pk = heap.top().pk;
    }
  };

  auto process = [&](int64_t pk, const Row& row) -> Status {
    Status m;
    if (!matchConditions(row, conds, m)) {
      if (!m.ok()) return m;
      return Status::Ok();
    }
    if (distance_topk) {
      const auto& vv = row.values[orders[0].col].v;
      const double d = use_avx2
                           ? l2Avx2Sq(vv.data(), orders[0].query.data(),
                                      std::min(vv.size(), orders[0].query.size()))
                           : l2DistanceSq(vv, orders[0].query);
      consider_hit(pk, row, d);
      return Status::Ok();
    }
    Candidate c;
    c.pk = pk;
    for (const auto& o : orders) {
      KeyValue kv;
      if (o.is_distance) {
        kv.kind = KeyValue::Kind::Double;
        kv.d = l2DistanceSq(row.values[o.col].v, o.query);
      } else {
        const Value& v = row.values[o.col];
        if (v.type == Value::Type::Int) {
          kv.kind = KeyValue::Kind::Int;
          kv.i = v.i;
        } else if (v.type == Value::Type::Str) {
          kv.kind = KeyValue::Kind::Str;
          kv.str = v.s;
        } else {
          return Status::Error(Err::kUnsupported,
                               "ORDER BY on VECTOR column is not supported");
        }
      }
      c.keys.push_back(std::move(kv));
    }
    if (order_topk) {
      if (candidate_heap.size() < k) {
        c.out = formatProjection(row, cols);
        candidate_heap.push(std::move(c));
      } else if (candidate_better(c, candidate_heap.top())) {
        c.out = formatProjection(row, cols);
        candidate_heap.pop();
        candidate_heap.push(std::move(c));
      }
    } else {
      c.out = formatProjection(row, cols);
      candidates.push_back(std::move(c));
      if (early_limit && candidates.size() >= k) stop_scan = true;
    }
    return Status::Ok();
  };

  if (!limit_zero) {
    bool cache_scanned = false;
    if (ann_quant) {
      QuantCache* qc = ensureQuant(*table, orders[0].col);
      if (qc != nullptr && qc->valid && qc->dim == orders[0].query.size() &&
          qc->q.size() == table->vector_cache.pks.size() * qc->dim) {
        std::vector<uint8_t> qv(qc->dim);
        for (size_t j = 0; j < qc->dim; ++j) {
          int q = static_cast<int>(
              std::lround((orders[0].query[j] - qc->minv) * qc->scale));
          qv[j] = static_cast<uint8_t>(std::max(0, std::min(255, q)));
        }
        const size_t ncand = std::min<size_t>(256, table->vector_cache.pks.size());
        using Cand = std::pair<int, size_t>;  // larger approximate distance is worse
        std::priority_queue<Cand> heap;
        for (size_t vi = 0; vi < table->vector_cache.pks.size(); ++vi) {
          const uint8_t* rowq = qc->q.data() + vi * qc->dim;
          const int d = quantDistanceSq(rowq, qv.data(), qc->dim);
          if (heap.size() < ncand) {
            heap.push({d, vi});
          } else if (d < heap.top().first) {
            heap.pop();
            heap.push({d, vi});
          }
        }
        const VectorCache& vc = table->vector_cache;
        while (!heap.empty()) {
          size_t vi = heap.top().second;
          heap.pop();
          const Row& row = *vc.rows[vi];
          const double d = use_avx2
                               ? l2Avx2Sq(vc.vec.data() + vi * vc.dim,
                                          orders[0].query.data(), vc.dim)
                               : l2DistanceSq(vc.vec.data() + vi * vc.dim,
                                              orders[0].query.data(), vc.dim);
          consider_hit(vc.pks[vi], row, d);
        }
        cache_scanned = true;
      }
    }
    if (!cache_scanned && distance_topk && !table->disk_mode) {
      static const std::vector<int64_t> kNoMatches;
      const std::vector<int64_t>* filtered = nullptr;
      for (const auto& cond : conds) {
        if (cond.op != "=") continue;
        FilterCache* fc = ensureFilterCache(*table, cond.col);
        const std::vector<int64_t>* list = nullptr;
        if (cond.literal.type == Value::Type::Str) {
          auto it = fc->str.find(cond.literal.s);
          if (it != fc->str.end()) list = &it->second;
        } else if (cond.literal.type == Value::Type::Int) {
          auto it = fc->num.find(cond.literal.i);
          if (it != fc->num.end()) list = &it->second;
        }
        if (list == nullptr) list = &kNoMatches;
        if (filtered == nullptr || list->size() < filtered->size()) {
          filtered = list;
        }
      }
      if (filtered != nullptr) {
        const float* q = orders[0].query.data();
        for (int64_t pk : *filtered) {
          auto it = table->rows.find(pk);
          if (it == table->rows.end()) continue;
          const Row& row = it->second;
          Status m;
          if (!conds.empty() && !matchConditions(row, conds, m)) {
            if (!m.ok()) return m;
            continue;
          }
          const auto& vv = row.values[orders[0].col].v;
          const size_t dim = std::min(vv.size(), orders[0].query.size());
          const double d = use_avx2 ? l2Avx2Sq(vv.data(), q, dim)
                                    : l2DistanceSq(vv, orders[0].query);
          consider_hit(pk, row, d);
        }
        cache_scanned = true;
      }
      if (!cache_scanned) {
        ensureVectorCache(*table, orders[0].col);
        const VectorCache& cache = table->vector_cache;
        if (cache.valid && cache.dim == orders[0].query.size()) {
          const float* q = orders[0].query.data();
          const size_t n = cache.pks.size();
          if (conds.empty() && n >= 2000 && k > 0 &&
              std::thread::hardware_concurrency() >= 2) {
            auto scan = [&](size_t begin, size_t end,
                            std::priority_queue<Hit, std::vector<Hit>,
                                                decltype(worse)>& local) {
              for (size_t i = begin; i < end; ++i) {
                const double d = use_avx2
                                     ? l2Avx2Sq(cache.vec.data() + i * cache.dim,
                                                q, cache.dim)
                                     : l2DistanceSq(cache.vec.data() + i * cache.dim,
                                                    q, cache.dim);
                if (local.size() < k) {
                  Hit h;
                  h.d = d;
                  h.pk = cache.pks[i];
                  h.row = *cache.rows[i];
                  local.push(std::move(h));
                } else {
                  Hit probe;
                  probe.d = d;
                  probe.pk = cache.pks[i];
                  if (worse(probe, local.top())) {
                    Hit h;
                    h.d = d;
                    h.pk = cache.pks[i];
                    h.row = *cache.rows[i];
                    local.pop();
                    local.push(std::move(h));
                  }
                }
              }
            };
            std::priority_queue<Hit, std::vector<Hit>, decltype(worse)> left(worse), right(worse);
            const size_t mid = n / 2;
            std::thread t1(scan, 0, mid, std::ref(left));
            std::thread t2(scan, mid, n, std::ref(right));
            t1.join();
            t2.join();
            while (!left.empty()) {
              consider_hit(left.top().pk, left.top().row, left.top().d);
              left.pop();
            }
            while (!right.empty()) {
              consider_hit(right.top().pk, right.top().row, right.top().d);
              right.pop();
            }
          } else {
            Status m;
            for (size_t i = 0; i < n; ++i) {
              const Row& row = *cache.rows[i];
              if (!conds.empty() && !matchConditions(row, conds, m)) {
                if (!m.ok()) return m;
                continue;
              }
              const double d = use_avx2
                                   ? l2Avx2Sq(cache.vec.data() + i * cache.dim, q,
                                              cache.dim)
                                   : l2DistanceSq(cache.vec.data() + i * cache.dim,
                                                  q, cache.dim);
              consider_hit(cache.pks[i], row, d);
            }
          }
          cache_scanned = true;
        }
      }
    }
    if (!cache_scanned) {
      if (table->disk_mode) {
        Status row_status = Status::Ok();
        Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
          if (v.type != static_cast<uint8_t>(WalType::Insert) &&
              v.type != static_cast<uint8_t>(WalType::Update)) {
            return true;
          }
          if (v.table != table->schema.table_id) return true;
          auto it = table->locations.find(static_cast<int64_t>(v.pk));
          if (it == table->locations.end() || it->second != v.offset) return true;
          Row row;
          std::string err;
          if (!decodeRow(schema, v.payload, v.payload_size, row, err)) {
            row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
            return false;
          }
          Status s = process(static_cast<int64_t>(v.pk), row);
          if (!s.ok()) {
            row_status = s;
            return false;
          }
          return !stop_scan;
        }, nullptr, false);
        if (!scan.ok()) return scan;
        if (!row_status.ok()) return row_status;
      } else {
        for (const auto& [pk, row] : table->rows) {
          Status s = process(pk, row);
          if (!s.ok()) return s;
          if (stop_scan) break;
        }
      }
    }
  }

  out.has_result = true;
  fill_result_columns(out);

  if (distance_topk) {
    std::vector<Hit> hits;
    hits.reserve(heap.size());
    while (!heap.empty()) {
      hits.push_back(heap.top());
      heap.pop();
    }
    std::sort(hits.begin(), hits.end(), worse);
    out.rows.reserve(hits.size());
    for (const auto& h : hits) out.rows.push_back(formatProjection(h.row, cols));
    return Status::Ok();
  }
  if (order_topk) {
    std::vector<Candidate> top;
    top.reserve(candidate_heap.size());
    while (!candidate_heap.empty()) {
      top.push_back(candidate_heap.top());
      candidate_heap.pop();
    }
    std::sort(top.begin(), top.end(), candidate_better);
    out.rows.reserve(top.size());
    for (auto& c : top) out.rows.push_back(std::move(c.out));
    return Status::Ok();
  }
  if (!orders.empty()) {
    std::stable_sort(candidates.begin(), candidates.end(), candidate_better);
  }
  if (stmt.has_limit && candidates.size() > stmt.limit) {
    candidates.resize(static_cast<size_t>(stmt.limit));
  }
  out.rows.reserve(candidates.size());
  for (auto& c : candidates) out.rows.push_back(std::move(c.out));
  return Status::Ok();
}

Status Executor::execute(const Statement& stmt, ExecResult& out) {
  out = ExecResult{};
  switch (stmt.kind) {
    case Statement::Kind::Begin:
      if (active_) {
        return Status::Error(Err::kTxnState, "nested transactions are not supported");
      }
      return beginTxn();
    case Statement::Kind::Commit:
      return commitTxn();
    case Statement::Kind::Rollback:
      return rollbackTxn();
    case Statement::Kind::Exit:
      return Status::Ok();
    case Statement::Kind::Select:
      return executeSelect(stmt.select, out);
    case Statement::Kind::CreateTable:
      return executeCreateTable(stmt.create_table);
    case Statement::Kind::DropTable:
      return executeDropTable(stmt.drop_table);
    case Statement::Kind::TruncateTable:
      return executeTruncateTable(stmt.truncate_table);
    case Statement::Kind::CreateIndex:
      return executeCreateIndex(stmt.create_index);
    case Statement::Kind::Set: {
      std::string key = toLower(stmt.set.key);
      if (key == "__mvdb_sync__") {
        out.sync_marker = true;
        return Status::Ok();
      }
      const bool is_query_mode =
          key == "query_mode" || key == "mode" ||
          (key.size() > 11 && key.ends_with(".query_mode"));
      if (is_query_mode) {
        std::string v = toLower(stmt.set.value);
        if (v != "exact" && v != "ann") {
          return Status::Error(Err::kInvalidValue, "query_mode must be exact or ann");
        }
        query_mode_ = v;
      }
      // Other session settings (SET NAMES, autocommit, sql_mode, ...) are
      // accepted as no-ops; the single-database task does not need them.
      return Status::Ok();
    }
    case Statement::Kind::Use:
      // Single database: USE is accepted as a no-op.
      return Status::Ok();
    case Statement::Kind::Insert:
    case Statement::Kind::Update:
    case Statement::Kind::Delete: {
      bool implicit = !active_;
      if (implicit) {
        Status st = beginTxn();
        if (!st.ok()) return st;
      }
      size_t stmt_undo = undo_.size();
      uint64_t stmt_wal = db_.wal().size();
      uint64_t saved_op_count = op_count_;
      uint64_t saved_first_lsn = first_lsn_;
      uint64_t saved_last_lsn = last_lsn_;
      Status st = Status::Ok();
      if (stmt.kind == Statement::Kind::Insert) {
        st = executeInsert(stmt.insert);
        if (st.ok()) out.affected = stmt.insert.rows.size();
      } else if (stmt.kind == Statement::Kind::Update) {
        st = executeUpdate(stmt.update, out);
      } else {
        st = executeDelete(stmt.del, out);
      }
      if (!st.ok()) {
        if (implicit) {
          Status rb = rollbackTxn();
          if (!rb.ok()) return rb;
        } else {
          Status rb = restoreTo(stmt_undo, stmt_wal);
          if (!rb.ok()) return rb;
          op_count_ = static_cast<uint32_t>(saved_op_count);
          first_lsn_ = saved_first_lsn;
          last_lsn_ = saved_last_lsn;
        }
        return st;
      }
      if (implicit) {
        Status c = commitTxn();
        if (!c.ok()) return c;
      }
      return Status::Ok();
    }
    case Statement::Kind::None:
      return Status::Error(Err::kSyntax, "empty statement");
  }
  return Status::Error(Err::kUnsupported, "unsupported statement");
}

}  // namespace mvdb
