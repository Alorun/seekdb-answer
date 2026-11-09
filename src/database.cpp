#include "database.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <set>

namespace mvdb {

namespace {
constexpr size_t kHeaderSize = 44;
constexpr uint32_t kMaxPayload = 64u * 1024u * 1024u;

void putU32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
void putU64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}
uint32_t getU32(const unsigned char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
  return v;
}
uint64_t getU64(const unsigned char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}
}  // namespace

Wal::~Wal() {
  if (fd_ >= 0) ::close(fd_);
}

Status Wal::open(const std::string& path) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    return Status::Error(Err::kIo, "cannot open WAL " + path + ": " +
                                        std::strerror(errno));
  }
  off_t end = ::lseek(fd_, 0, SEEK_END);
  if (end < 0) {
    return Status::Error(Err::kIo, "cannot seek WAL: " + std::string(std::strerror(errno)));
  }
  size_ = static_cast<uint64_t>(end);
  return Status::Ok();
}

Status Wal::writeAll(const char* data, size_t n) {
  size_t done = 0;
  while (done < n) {
    ssize_t w = ::write(fd_, data + done, n - done);
    if (w < 0) {
      if (errno == EINTR) continue;
      return Status::Error(Err::kIo, "WAL write failed: " + std::string(std::strerror(errno)));
    }
    if (w == 0) return Status::Error(Err::kIo, "WAL write returned 0");
    done += static_cast<size_t>(w);
  }
  return Status::Ok();
}

Status Wal::append(const WalRecord& rec) {
  if (rec.payload.size() > kMaxPayload) {
    return Status::Error(Err::kIo, "record too large");
  }
  const uint64_t start = size_;
  std::string header;
  header.reserve(kHeaderSize);
  putU32(header, static_cast<uint32_t>(rec.payload.size()));
  putU32(header, crc32c(rec.payload.data(), rec.payload.size()));
  header.push_back(static_cast<char>(rec.type));
  header.push_back(static_cast<char>(0));
  header.push_back(0);
  header.push_back(0);
  putU64(header, rec.lsn);
  putU64(header, rec.txn);
  putU64(header, rec.table);
  putU64(header, rec.pk);

  Status st = writeAll(header.data(), header.size());
  if (!st.ok()) return st;
  st = writeAll(rec.payload.data(), rec.payload.size());
  if (!st.ok()) {
    if (::ftruncate(fd_, static_cast<off_t>(start)) != 0) {
      // Best effort: the caller already has an I/O error to report.
    }
    if (::lseek(fd_, static_cast<off_t>(start), SEEK_SET) < 0) {
      // Best effort: the caller already has an I/O error to report.
    }
    size_ = start;
    return st;
  }
  size_ = start + kHeaderSize + rec.payload.size();
  return Status::Ok();
}

Status Wal::appendCommit(uint64_t txn, uint32_t op_count, uint64_t first_lsn,
                         uint64_t last_lsn, uint64_t lsn) {
  Writer w;
  w.u32(op_count);
  w.u64(first_lsn);
  w.u64(last_lsn);
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::Commit);
  rec.lsn = lsn;
  rec.txn = txn;
  rec.payload = w.take();
  return append(rec);
}

Status Wal::sync() {
  if (fd_ < 0) return Status::Error(Err::kIo, "WAL is not open");
  if (::fdatasync(fd_) != 0) {
    return Status::Error(Err::kIo, "fdatasync failed: " + std::string(std::strerror(errno)));
  }
  return Status::Ok();
}

Status Wal::truncate(uint64_t size) {
  if (fd_ < 0) return Status::Error(Err::kIo, "WAL is not open");
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
    return Status::Error(Err::kIo, "ftruncate failed: " + std::string(std::strerror(errno)));
  }
  if (::lseek(fd_, static_cast<off_t>(size), SEEK_SET) < 0) {
    return Status::Error(Err::kIo, "lseek failed: " + std::string(std::strerror(errno)));
  }
  size_ = size;
  return Status::Ok();
}

Wal::ReadResult Wal::readRecord(uint64_t offset, WalRecord& out,
                                uint64_t& next_offset, std::string& err) const {
  unsigned char header[kHeaderSize];
  size_t got = 0;
  while (got < kHeaderSize) {
    ssize_t n = ::pread(fd_, header + got, kHeaderSize - got,
                        static_cast<off_t>(offset + got));
    if (n < 0) {
      if (errno == EINTR) continue;
      err = "pread failed: " + std::string(std::strerror(errno));
      return ReadResult::IoError;
    }
    if (n == 0) {
      if (got == 0) return ReadResult::Eof;
      err = "partial WAL header";
      return ReadResult::Invalid;
    }
    got += static_cast<size_t>(n);
  }
  uint32_t len = getU32(header);
  uint32_t crc = getU32(header + 4);
  uint8_t type = header[8];
  uint64_t lsn = getU64(header + 12);
  uint64_t txn = getU64(header + 20);
  uint64_t table = getU64(header + 28);
  uint64_t pk = getU64(header + 36);
  if (len > kMaxPayload) {
    err = "WAL record payload too large";
    return ReadResult::Invalid;
  }
  std::string payload;
  payload.resize(len);
  size_t pgot = 0;
  while (pgot < len) {
    ssize_t n = ::pread(fd_, payload.data() + pgot, len - pgot,
                        static_cast<off_t>(offset + kHeaderSize + pgot));
    if (n < 0) {
      if (errno == EINTR) continue;
      err = "pread failed: " + std::string(std::strerror(errno));
      return ReadResult::IoError;
    }
    if (n == 0) {
      err = "partial WAL payload";
      return ReadResult::Invalid;
    }
    pgot += static_cast<size_t>(n);
  }
  if (crc32c(payload.data(), payload.size()) != crc) {
    err = "WAL checksum mismatch";
    return ReadResult::Invalid;
  }
  out.type = type;
  out.lsn = lsn;
  out.txn = txn;
  out.table = table;
  out.pk = pk;
  out.payload = std::move(payload);
  next_offset = offset + kHeaderSize + len;
  return ReadResult::Ok;
}

Status Database::ensureDirectory() {
  if (::mkdir(data_dir_.c_str(), 0755) != 0 && errno != EEXIST) {
    return Status::Error(Err::kIo, "cannot create data dir " + data_dir_ + ": " +
                                        std::strerror(errno));
  }
  struct stat st {};
  if (::stat(data_dir_.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    return Status::Error(Err::kIo, "data dir is not a directory: " + data_dir_);
  }
  return Status::Ok();
}

Status Database::open() {
  Status st = ensureDirectory();
  if (!st.ok()) return st;
  wal_ = std::make_unique<Wal>();
  st = wal_->open(data_dir_ + "/wal.log");
  if (!st.ok()) return st;
  st = recover();
  if (!st.ok()) return st;
  // Make the WAL file's directory entry durable after creation.
  int dfd = ::open(data_dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return Status::Ok();
}

Table* Database::findTable(const std::string& name) {
  auto it = by_name_.find(toLower(name));
  return it == by_name_.end() ? nullptr : it->second;
}

Table* Database::tableById(uint64_t id) {
  auto it = by_id_.find(id);
  return it == by_id_.end() ? nullptr : it->second;
}

Status Database::addTable(std::unique_ptr<Table> table) {
  std::string key = toLower(table->schema.name);
  if (by_name_.count(key)) {
    return Status::Error(Err::kTableExists, "table already exists: " + table->schema.name);
  }
  Table* ptr = table.get();
  tables_.push_back(std::move(table));
  by_name_[key] = ptr;
  by_id_[ptr->schema.table_id] = ptr;
  return Status::Ok();
}

bool Database::indexExists(const std::string& name) const {
  return indexes_.count(toLower(name)) != 0;
}

Status Database::createIndex(const std::string& name, const std::string& table,
                             const std::string& column) {
  std::string key = toLower(name);
  if (indexes_.count(key)) {
    return Status::Error(Err::kIndexExists, "index already exists: " + name);
  }
  Table* t = findTable(table);
  if (!t) return Status::Error(Err::kTableNotFound, "table not found: " + table);
  int found = -1;
  for (size_t i = 0; i < t->schema.columns.size(); ++i) {
    if (iequals(t->schema.columns[i].name, column)) {
      found = static_cast<int>(i);
      if (t->schema.columns[i].type != ColumnType::Vector) {
        return Status::Error(Err::kInvalidValue, "index column is not VECTOR: " + column);
      }
      break;
    }
  }
  if (found < 0) return Status::Error(Err::kColumnNotFound, "column not found: " + column);
  IndexInfo info;
  info.table_id = t->schema.table_id;
  info.column = static_cast<uint32_t>(found);
  indexes_[key] = info;
  return Status::Ok();
}

Status Database::applyRecord(const WalRecord& rec) {
  switch (static_cast<WalType>(rec.type)) {
    case WalType::CreateTable: {
      Schema schema;
      std::string err;
      if (!decodeSchema(rec.payload, schema, err)) {
        return Status::Error(Err::kIo, "recovery: " + err);
      }
      schema.table_id = rec.table;
      auto table = std::make_unique<Table>();
      table->schema = std::move(schema);
      Status st = addTable(std::move(table));
      if (!st.ok()) return st;
      return Status::Ok();
    }
    case WalType::Insert:
    case WalType::Update: {
      Table* table = tableById(rec.table);
      if (!table) {
        return Status::Error(Err::kIo, "recovery: missing table id " + std::to_string(rec.table));
      }
      Row row;
      std::string err;
      if (!decodeRow(table->schema, rec.payload, row, err)) {
        return Status::Error(Err::kIo, "recovery: " + err);
      }
      table->rows[static_cast<int64_t>(rec.pk)] = std::move(row);
      return Status::Ok();
    }
    case WalType::Delete: {
      Table* table = tableById(rec.table);
      if (!table) {
        return Status::Error(Err::kIo, "recovery: missing table id " + std::to_string(rec.table));
      }
      table->rows.erase(static_cast<int64_t>(rec.pk));
      return Status::Ok();
    }
    case WalType::CreateIndex: {
      Table* table = tableById(rec.table);
      if (!table) {
        return Status::Error(Err::kIo, "recovery: missing table id " + std::to_string(rec.table));
      }
      std::string name;
      uint32_t column = 0;
      std::string err;
      if (!decodeIndex(rec.payload, name, column, err)) {
        return Status::Error(Err::kIo, "recovery: " + err);
      }
      if (column >= table->schema.columns.size()) {
        return Status::Error(Err::kIo, "recovery: bad index column");
      }
      std::string key = toLower(name);
      if (!indexes_.count(key)) {
        IndexInfo info;
        info.table_id = rec.table;
        info.column = column;
        indexes_[key] = info;
      }
      return Status::Ok();
    }
    case WalType::Commit:
      return Status::Ok();
  }
  return Status::Error(Err::kIo, "recovery: unknown record type");
}

Status Database::recover() {
  std::unordered_map<uint64_t, std::vector<WalRecord>> pending;
  std::vector<uint64_t> commit_order;
  std::set<uint64_t> seen_commit;
  uint64_t offset = 0;
  uint64_t last_commit_end = 0;
  uint64_t max_lsn = 0;
  uint64_t max_txn = 0;
  uint64_t max_table = 0;

  while (true) {
    WalRecord rec;
    uint64_t next = 0;
    std::string err;
    auto r = wal_->readRecord(offset, rec, next, err);
    if (r == Wal::ReadResult::Eof) break;
    if (r == Wal::ReadResult::IoError) {
      return Status::Error(Err::kIo, "recovery: " + err);
    }
    if (r == Wal::ReadResult::Invalid) {
      // Crash-truncated tail: keep everything up to the last commit.
      break;
    }
    max_lsn = std::max(max_lsn, rec.lsn);
    max_txn = std::max(max_txn, rec.txn);
    max_table = std::max(max_table, rec.table);
    if (static_cast<WalType>(rec.type) == WalType::Commit) {
      if (seen_commit.insert(rec.txn).second) {
        commit_order.push_back(rec.txn);
      }
      last_commit_end = next;
    } else {
      pending[rec.txn].push_back(std::move(rec));
    }
    offset = next;
  }

  for (uint64_t txn : commit_order) {
    auto it = pending.find(txn);
    if (it == pending.end()) continue;
    for (const auto& rec : it->second) {
      Status st = applyRecord(rec);
      if (!st.ok()) return st;
    }
  }

  if (last_commit_end < wal_->size()) {
    Status st = wal_->truncate(last_commit_end);
    if (!st.ok()) return st;
  }

  next_lsn_ = max_lsn + 1;
  next_txn_id_ = max_txn + 1;
  next_table_id_ = max_table + 1;
  if (next_lsn_ == 0) next_lsn_ = 1;
  if (next_txn_id_ == 0) next_txn_id_ = 1;
  if (next_table_id_ == 0) next_table_id_ = 1;
  return Status::Ok();
}

}  // namespace mvdb
