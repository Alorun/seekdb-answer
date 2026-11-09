#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

namespace mvdb {

struct Condition {
  std::string column;
  std::string op;  // "=", "<", "<=", ">", ">=", "!="
  Value value;
};

struct SelectItem {
  enum class Kind { Star, Column, Distance } kind = Kind::Column;
  std::string column;       // Column or vector column
  std::string vector_text;  // Distance: the literal text
  std::string alias;
  std::string text;  // expression text for header fallback
};

struct OrderItem {
  std::string column;
  bool desc = false;
};

struct SelectStmt {
  std::string table;
  std::vector<SelectItem> items;
  std::vector<Condition> where;
  std::vector<OrderItem> order;
  bool has_limit = false;
  uint64_t limit = 0;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;  // empty = all columns
  std::vector<std::vector<Value>> rows;
};

struct CreateTableStmt {
  std::string table;
  std::vector<Column> columns;
  int pk_index = -1;
};

struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, Value>> sets;
  std::vector<Condition> where;
};

struct DeleteStmt {
  std::string table;
  std::vector<Condition> where;
};

struct CreateIndexStmt {
  std::string name;
  std::string table;
  std::string column;
};

struct SetStmt {
  std::string key;
  std::string value;
};

struct Statement {
  enum class Kind {
    None,
    CreateTable,
    CreateIndex,
    Set,
    Insert,
    Select,
    Update,
    Delete,
    Begin,
    Commit,
    Rollback,
    Exit
  } kind = Kind::None;

  CreateTableStmt create_table;
  CreateIndexStmt create_index;
  SetStmt set;
  InsertStmt insert;
  SelectStmt select;
  UpdateStmt update;
  DeleteStmt del;
};

struct ParseResult {
  enum class Kind { Ok, NeedMore, Error, Empty } kind = Kind::Error;
  Statement stmt;
  Status status;
  size_t consumed = 0;

  static ParseResult MakeOk(Statement s, size_t c) {
    ParseResult r;
    r.kind = Kind::Ok;
    r.stmt = std::move(s);
    r.consumed = c;
    return r;
  }
  static ParseResult MakeNeedMore() {
    ParseResult r;
    r.kind = Kind::NeedMore;
    return r;
  }
  static ParseResult MakeEmpty() {
    ParseResult r;
    r.kind = Kind::Empty;
    return r;
  }
  static ParseResult MakeError(Status st) {
    ParseResult r;
    r.kind = Kind::Error;
    r.status = std::move(st);
    return r;
  }
};

// Parse one statement from the beginning of `input`. If the input ends in the
// middle of a statement, returns NeedMore so the caller can read more bytes.
ParseResult parseStatement(const std::string& input);

}  // namespace mvdb
