#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "database.h"
#include "parser.h"

namespace mvdb {

struct ExecResult {
  bool has_result = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  uint64_t affected = 0;
};

class Executor {
 public:
  explicit Executor(Database& db) : db_(db) {}

  Status execute(const Statement& stmt, ExecResult& out);
  Status shutdown();

 private:
  struct Undo {
    uint64_t table_id = 0;
    int64_t pk = 0;
    bool had_old = false;
    Row old_row;
  };

  Status beginTxn();
  Status commitTxn();
  Status rollbackTxn();
  Status restoreTo(size_t undo_size, uint64_t wal_offset);

  Status executeSelect(const SelectStmt& stmt, ExecResult& out);
  Status executeInsert(const InsertStmt& stmt);
  Status executeUpdate(const UpdateStmt& stmt, ExecResult& out);
  Status executeDelete(const DeleteStmt& stmt, ExecResult& out);
  Status executeCreateTable(const CreateTableStmt& stmt);
  Status executeCreateIndex(const CreateIndexStmt& stmt);

  Status applyInsert(Table& table, int64_t pk, Row row);
  Status applyUpdate(Table& table, int64_t pk, Row row);
  Status applyDelete(Table& table, int64_t pk);

  Status writeRowRecord(WalType type, const Table& table, int64_t pk,
                        const Row& row);
  Status writeCommit();
  Status handleStatementFailure(const Status& failure, bool implicit,
                                size_t undo_size, uint64_t wal_offset);

  Database& db_;
  bool active_ = false;
  uint64_t txn_id_ = 0;
  uint64_t txn_start_ = 0;
  uint64_t first_lsn_ = 0;
  uint64_t last_lsn_ = 0;
  uint32_t op_count_ = 0;
  std::vector<Undo> undo_;
  std::string query_mode_ = "exact";
};

// Helpers shared with tests.
int findColumn(const Schema& schema, const std::string& name);
bool rowMatches(const Table& table, const Row& row,
                const std::vector<Condition>& where, std::string& err);
Status convertValue(const Column& col, const Value& input, Value& out);
double l2Distance(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace mvdb
