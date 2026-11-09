#pragma once

#include <cstdint>
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
};

struct WalRecord {
  uint8_t type = 0;
  uint64_t lsn = 0;
  uint64_t txn = 0;
  uint64_t table = 0;
  uint64_t pk = 0;
  std::string payload;
};

class Wal {
 public:
  Wal() = default;
  ~Wal();
  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  Status open(const std::string& path);
  Status append(const WalRecord& rec);
  Status appendCommit(uint64_t txn, uint32_t op_count, uint64_t first_lsn,
                      uint64_t last_lsn, uint64_t lsn);
  Status sync();
  Status truncate(uint64_t size);
  uint64_t size() const { return size_; }

  enum class ReadResult { Ok, Eof, Invalid, IoError };
  ReadResult readRecord(uint64_t offset, WalRecord& out, uint64_t& next_offset,
                        std::string& err) const;

 private:
  Status writeAll(const char* data, size_t n);

  int fd_ = -1;
  uint64_t size_ = 0;
  std::string path_;
};

struct Table {
  Schema schema;
  std::map<int64_t, Row> rows;
};

class Database {
 public:
  explicit Database(std::string data_dir) : data_dir_(std::move(data_dir)) {}

  Status open();
  Table* findTable(const std::string& name);
  Table* tableById(uint64_t id);
  Status addTable(std::unique_ptr<Table> table);
  Status createIndex(const std::string& name, const std::string& table,
                     const std::string& column);
  bool indexExists(const std::string& name) const;

  Wal& wal() { return *wal_; }
  uint64_t newTableId() { return next_table_id_++; }
  uint64_t newTxnId() { return next_txn_id_++; }
  uint64_t newLsn() { return next_lsn_++; }
  const std::string& dataDir() const { return data_dir_; }

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
  std::unordered_map<std::string, IndexInfo> indexes_;
  uint64_t next_table_id_ = 1;
  uint64_t next_txn_id_ = 1;
  uint64_t next_lsn_ = 1;
};

}  // namespace mvdb
