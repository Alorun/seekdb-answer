#include "executor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <immintrin.h>
#include <queue>
#include <set>
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
  bool is_num = true;
  double num = 0;
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

double l2Scalar(const float* a, const float* b, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

__attribute__((target("avx2,fma"))) double l2Avx2(const float* a, const float* b,
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
  float sum = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] + tmp[6] +
              tmp[7];
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return std::sqrt(static_cast<double>(sum));
}

using L2Fn = double (*)(const float*, const float*, size_t);
L2Fn pickL2() {
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    return l2Avx2;
  }
  return l2Scalar;
}

}  // namespace

double l2Distance(const std::vector<float>& a, const std::vector<float>& b) {
  static const L2Fn fn = pickL2();
  const size_t n = std::min(a.size(), b.size());
  return fn(a.data(), b.data(), n);
}

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
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  u.old_row = std::move(old);
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
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  u.old_row = std::move(old);
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
  });
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

Status Executor::executeCreateIndex(const CreateIndexStmt& stmt) {
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  if (db_.indexExists(stmt.name)) {
    return Status::Error(Err::kIndexExists, "index already exists: " + stmt.name);
  }
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
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

  const bool topk = stmt.has_limit && stmt.limit > 0 && orders.size() == 1 &&
                    orders[0].is_distance && !orders[0].desc;

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
  const size_t k = topk ? static_cast<size_t>(stmt.limit) : 0;
  std::vector<Candidate> candidates;

  auto process = [&](int64_t pk, const Row& row) -> Status {
    Status m;
    if (!matchConditions(row, conds, m)) {
      if (!m.ok()) return m;
      return Status::Ok();
    }
    if (topk) {
      Hit h;
      h.d = l2Distance(row.values[orders[0].col].v, orders[0].query);
      h.pk = pk;
      h.row = row;
      if (heap.size() < k) {
        heap.push(std::move(h));
      } else if (worse(h, heap.top())) {
        heap.pop();
        heap.push(std::move(h));
      }
      return Status::Ok();
    }
    Candidate c;
    c.pk = pk;
    c.out = formatProjection(row, cols);
    for (const auto& o : orders) {
      KeyValue kv;
      if (o.is_distance) {
        kv.is_num = true;
        kv.num = l2Distance(row.values[o.col].v, o.query);
      } else {
        const Value& v = row.values[o.col];
        if (v.type == Value::Type::Int) {
          kv.is_num = true;
          kv.num = static_cast<double>(v.i);
        } else if (v.type == Value::Type::Str) {
          kv.is_num = false;
          kv.str = v.s;
        } else {
          return Status::Error(Err::kUnsupported,
                               "ORDER BY on VECTOR column is not supported");
        }
      }
      c.keys.push_back(std::move(kv));
    }
    candidates.push_back(std::move(c));
    return Status::Ok();
  };

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
      return true;
    });
    if (!scan.ok()) return scan;
    if (!row_status.ok()) return row_status;
  } else {
    for (const auto& [pk, row] : table->rows) {
      Status s = process(pk, row);
      if (!s.ok()) return s;
    }
  }

  out.has_result = true;
  for (const auto& col : cols) out.columns.push_back(col.header);

  if (topk) {
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

  if (!orders.empty()) {
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const Candidate& a, const Candidate& b) {
                       for (size_t i = 0; i < orders.size(); ++i) {
                         const KeyValue& x = a.keys[i];
                         const KeyValue& y = b.keys[i];
                         int cmp = 0;
                         if (x.is_num && y.is_num) {
                           cmp = (x.num < y.num) ? -1 : (x.num > y.num ? 1 : 0);
                         } else if (!x.is_num && !y.is_num) {
                           cmp = x.str.compare(y.str);
                         }
                         if (cmp != 0) return orders[i].desc ? (cmp > 0) : (cmp < 0);
                       }
                       return a.pk < b.pk;
                     });
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
    case Statement::Kind::CreateIndex:
      return executeCreateIndex(stmt.create_index);
    case Statement::Kind::Set: {
      if (!iequals(stmt.set.key, "query_mode") && !iequals(stmt.set.key, "mode")) {
        return Status::Error(Err::kUnsupported, "unsupported session variable: " + stmt.set.key);
      }
      std::string v = toLower(stmt.set.value);
      if (v != "exact" && v != "ann") {
        return Status::Error(Err::kInvalidValue, "query_mode must be exact or ann");
      }
      query_mode_ = v;
      return Status::Ok();
    }
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
