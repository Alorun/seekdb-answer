#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common.h"

namespace mvdb {

enum class WalType : uint8_t {
  CreateTable = 1,
  Insert = 2,
  Update = 3,
  Delete = 4,
  Commit = 5,
  CreateIndex = 6,
  DropTable = 7,
  TruncateTable = 8,
};

struct WalRecord {
  uint8_t type = 0;
  uint64_t lsn = 0;
  uint64_t txn = 0;
  uint64_t table = 0;
  uint64_t pk = 0;
  uint64_t offset = 0;
  std::string payload;
};

struct WalRecordView {
  uint8_t type = 0;
  uint64_t lsn = 0;
  uint64_t txn = 0;
  uint64_t table = 0;
  uint64_t pk = 0;
  uint64_t offset = 0;
  const char* payload = nullptr;
  size_t payload_size = 0;
};

class Wal {
 public:
  Wal() = default;
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  Status open(const std::string& path);
  Status append(const WalRecord& rec, uint64_t* out_offset = nullptr);
  // Encode one record (header + payload) into `out`; the caller can append a
  // whole multi-row INSERT as one write syscall via appendBatch.
  void encodeRecord(const WalRecord& rec, std::string& out) const;
  Status appendBatch(const std::string& bytes, uint64_t* out_offset = nullptr);
  Status appendCommit(uint64_t txn, uint32_t op_count, uint64_t first_lsn,
                      uint64_t last_lsn, uint64_t lsn);
  Status sync();
  Status truncate(uint64_t size);
  uint64_t size() const { return size_; }

  enum class ReadResult { Ok, Eof, Invalid, IoError };
  ReadResult readRecord(uint64_t offset, WalRecord& out, uint64_t& next_offset,
                        std::string& err) const;

  // Sequential scan over the current file contents. The callback returns false
  // to stop early. `complete` is set to false if the file has an invalid tail.
  // `verify_crc=false` is for repeat scans after recovery has validated the
  // file; recovery/load always keep it true.
  Status forEachRecord(const std::function<bool(const WalRecordView&)>& fn,
                       bool* complete = nullptr, bool verify_crc = true) const;

 private:
  Status writeAll(const char* data, size_t n);
  Status writeAllV(struct iovec* iov, int iovcnt);

  int fd_ = -1;
  uint64_t size_ = 0;
  std::string path_;
};

// Read-only scan accelerator derived from Table::rows. Rebuilt lazily when
// row_version changes; it never owns logical state.
// Keep the in-memory row cache bounded by bytes as well as row count: a table
// with fewer than --memory-rows rows can still be too wide for the cgroup.
inline size_t estimatedRowBytes(const Schema& s) {
  size_t n = 32;
  for (const auto& c : s.columns) {
    if (c.type == ColumnType::BigInt) {
      n += 8;
    } else if (c.type == ColumnType::Varchar) {
      n += static_cast<size_t>(c.length) + 8;
    } else {
      n += static_cast<size_t>(c.length) * 4 + 8;
    }
  }
  return n;
}

constexpr size_t kCacheBudgetBytes = 64ull * 1024ull * 1024ull;

struct VectorCache {
  uint64_t version = 0;
  int vec_col = -1;
  size_t dim = 0;
  std::vector<int64_t> pks;
  std::vector<const Row*> rows;
  std::vector<float> vec;
  bool valid = false;
};

struct QuantCache {
  uint64_t version = 0;
  int col = -1;
  size_t dim = 0;
  float minv = 0;
  float scale = 1;
  std::vector<uint8_t> q;  // pks.size() * dim, row-major, 0..255
  bool valid = false;
};

// 7-bit symmetric scalar quantization of the VECTOR column for the fast
// scan. `scale` is chosen from the table-wide max |component|; `codes` holds
// n*dim values in [-63,63] and `errs[i]` is ||v_i - dequant(code_i)||, the
// triangle-inequality error budget used by the exact-mode lower bound.
struct Quant8Cache {
  uint64_t version = 0;
  int col = -1;
  size_t dim = 0;
  float scale = 1.0f;
  std::vector<int8_t> codes;
  std::vector<float> errs;
  bool valid = false;
};

struct FilterCache {
  uint64_t version = 0;
  int col = -1;
  bool valid = false;
  std::unordered_map<std::string, std::vector<int64_t>> str;
  std::unordered_map<int64_t, std::vector<int64_t>> num;
  // Same lists as vector-cache row indices (both are built in Table::rows
  // order).  The filtered TopK fast path uses these to avoid a std::map
  // lookup and a redundant condition check per candidate.
  std::unordered_map<std::string, std::vector<uint32_t>> str_idx;
  std::unordered_map<int64_t, std::vector<uint32_t>> num_idx;
};

struct IvfCache {
  uint64_t version = 0;
  int col = -1;
  size_t dim = 0;
  size_t nlist = 0;
  size_t nprobe = 0;
  std::vector<std::vector<float>> centroids;
  std::vector<std::vector<size_t>> lists;  // indices into Table::vector_cache
  bool valid = false;
};

struct Table {
  Schema schema;
  // Small tables keep rows in memory for fast scans. Large tables set
  // disk_mode and keep only the PK -> WAL offset index.
  std::map<int64_t, Row> rows;
  std::unordered_map<int64_t, uint64_t> locations;
  bool disk_mode = false;
  uint64_t row_version = 0;
  VectorCache vector_cache;
  std::unordered_map<int, FilterCache> filter_caches;
  std::unordered_map<int, IvfCache> ivf_caches;
  std::unordered_map<int, QuantCache> quant_caches;
  std::unordered_map<int, Quant8Cache> quant8_caches;
};

class Database {
 public:
  explicit Database(std::string data_dir, size_t memory_rows = 50000)
      : data_dir_(std::move(data_dir)), memory_rows_(memory_rows) {}

  Status open();
  Table* findTable(const std::string& name);
  Table* tableById(uint64_t id);
  Status addTable(std::unique_ptr<Table> table);
  Status dropTable(uint64_t id);
  Status createIndex(const std::string& name, const std::string& table,
                     const std::string& column);
  bool indexExists(const std::string& table, const std::string& name) const;

  Wal& wal() { return *wal_; }
  uint64_t newTableId() { return next_table_id_++; }
  uint64_t newTxnId() { return next_txn_id_++; }
  uint64_t newLsn() { return next_lsn_++; }
  const std::string& dataDir() const { return data_dir_; }
  size_t memoryRows() const { return memory_rows_; }
  Status loadSmallTables();
  Status checkpointIfNeeded(uint64_t threshold_bytes);
  Status checkpoint();

 private:
  Status recover();
  Status applyRecord(const WalRecord& rec);
  Status ensureDirectory();

  std::string data_dir_;
  std::unique_ptr<Wal> wal_;
  std::vector<std::unique_ptr<Table>> tables_;
  std::unordered_map<std::string, Table*> by_name_;
  std::unordered_map<uint64_t, Table*> by_id_;
  struct IndexInfo {
    uint64_t table_id = 0;
    uint32_t column = 0;
  };
  std::map<std::pair<uint64_t, std::string>, IndexInfo> indexes_;
  uint64_t next_table_id_ = 1;
  uint64_t next_txn_id_ = 1;
  uint64_t next_lsn_ = 1;
  size_t memory_rows_ = 50000;
};

}  // namespace mvdb
