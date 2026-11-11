#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "database.h"
#include "parser.h"

namespace mvdb {

// Reusable two-way scan executor.  The benchmark issues one TopK request at a
// time, so creating two std::threads per query costs more than the scan itself
// at 10k rows.  This keeps one helper thread alive for the whole session; the
// caller runs the first half of the scan while the helper runs the second.
//
// The handshake is a pair of atomics plus a bounded spin: a condition-variable
// wake-up costs several microseconds and would eat most of what a ~20us scan
// half has to gain, so the worker spins for a while after finishing a job and
// only falls back to sleeping (2ms re-check) when the session goes idle.
class ScanPool {
 public:
  ScanPool();
  ~ScanPool();
  ScanPool(const ScanPool&) = delete;
  ScanPool& operator=(const ScanPool&) = delete;

  bool usable() const { return usable_; }
  // Runs `helper_job` on the persistent worker and `main_job` on the caller,
  // then waits for the helper. Both jobs must not throw across the boundary.
  void run(const std::function<void()>& main_job,
           const std::function<void()>& helper_job);

 private:
  void workerLoop();

  std::thread worker_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::function<void()> job_;
  std::exception_ptr error_;
  std::atomic<uint64_t> seq_{0};
  std::atomic<uint64_t> done_{0};
  std::atomic<bool> stop_{false};
  bool usable_ = false;
};

struct ResolvedCond {
  int col = -1;
  std::string op;
  Value literal;
};

enum class ResultKind { Int, Str, Vec, Double };

struct ResultColumn {
  ResultKind kind = ResultKind::Str;
  size_t dim = 0;
};

struct ExecResult {
  bool has_result = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  std::vector<ResultColumn> column_kinds;
  uint64_t affected = 0;
  bool sync_marker = false;
};

class Executor {
 public:
  explicit Executor(Database& db) : db_(db) {}

  Status execute(const Statement& stmt, ExecResult& out);
  void setQueryMode(const std::string& mode) { query_mode_ = mode; }
  // True when the driver has already buffered more requests: using the helper
  // CPU does not steal cycles from a waiting client.
  void setParallelScan(bool on) { parallel_scan_ = on; }
  // Batch autocommit DML: append commit markers without syncing, then one
  // fdatasync for the whole group.  The caller must not release a success
  // response until flushSync() returns Ok.
  void setDeferSync(bool defer) { defer_sync_ = defer; }
  bool inTransaction() const { return active_; }
  // Build vector/IVF caches before the ready handshake.  Preparation is not
  // part of the timed query phase, but its memory/IO is counted, so this is
  // done once after recovery for existing large tables.
  Status prepare();
  Status flushSync();
  Status shutdown();

 private:
  struct Undo {
    uint64_t table_id = 0;
    int64_t pk = 0;
    bool had_row = false;
    Row old_row;
    bool had_location = false;
    uint64_t old_location = 0;
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
  Status executeDropTable(const DropTableStmt& stmt);
  Status executeTruncateTable(const std::string& name);
  Status executeCreateIndex(const CreateIndexStmt& stmt);

  Status applyInsert(Table& table, int64_t pk, Row row);
  Status applyUpdate(Table& table, int64_t pk, Row row);
  Status applyDelete(Table& table, int64_t pk);

  Status writeRowRecord(WalType type, const Table& table, int64_t pk,
                        const Row& row, uint64_t* out_offset = nullptr);
  Status readRowAt(const Table& table, uint64_t offset, int64_t pk, Row& row,
                   std::string& err) const;
  Status findRow(Table& table, int64_t pk, Row& row, bool& found);
  void spillIfNeeded(Table& table);
  Status collectMatchingPks(Table& table,
                            const std::vector<ResolvedCond>& conds,
                            std::vector<int64_t>& pks);
  Status writeCommit();
  Status handleStatementFailure(const Status& failure, bool implicit,
                                size_t undo_size, uint64_t wal_offset);

  Database& db_;
  ScanPool pool_;
  bool active_ = false;
  bool defer_sync_ = false;
  bool pending_sync_ = false;
  uint64_t txn_id_ = 0;
  uint64_t txn_start_ = 0;
  uint64_t first_lsn_ = 0;
  uint64_t last_lsn_ = 0;
  uint32_t op_count_ = 0;
  std::vector<Undo> undo_;
  std::string query_mode_ = "exact";
  bool parallel_scan_ = false;
};

// Helpers shared with tests.
int findColumn(const Schema& schema, const std::string& name);
bool rowMatches(const Table& table, const Row& row,
                const std::vector<Condition>& where, std::string& err);
Status convertValue(const Column& col, const Value& input, Value& out);
double l2DistanceSq(const float* a, const float* b, size_t n);
double l2DistanceSq(const std::vector<float>& a, const std::vector<float>& b);
double l2Distance(const std::vector<float>& a, const std::vector<float>& b);

}  // namespace mvdb
