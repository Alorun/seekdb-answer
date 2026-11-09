#include "parser.h"

#include <cctype>
#include <stdexcept>

namespace mvdb {

namespace {

struct NeedMore : std::exception {};
struct ParseError : std::exception {
  explicit ParseError(Status s) : status(std::move(s)) {}
  Status status;
};

struct Token {
  enum class Kind { End, Ident, Number, String, Punct, Invalid } kind = Kind::End;
  std::string text;
  size_t start = 0;
  size_t end = 0;
  char punct = 0;
  bool need_more = false;
};

class Lexer {
 public:
  explicit Lexer(const std::string& input) : input_(input) {}

  std::vector<Token> run() {
    std::vector<Token> out;
    size_t i = 0;
    const size_t n = input_.size();
    while (i < n) {
      unsigned char c = static_cast<unsigned char>(input_[i]);
      if (std::isspace(c)) {
        ++i;
        continue;
      }
      if (c == '-' && i + 1 < n && input_[i + 1] == '-') {
        size_t k = i + 2;
        if (k < n && !std::isspace(static_cast<unsigned char>(input_[k])) &&
            input_[k] != '-') {
          // MySQL requires whitespace after --; otherwise it is subtraction.
        } else {
          while (k < n && input_[k] != '\n') ++k;
          i = k;
          continue;
        }
      }
      if (c == '#') {
        size_t k = i + 1;
        while (k < n && input_[k] != '\n') ++k;
        i = k;
        continue;
      }
      if (c == '/' && i + 1 < n && input_[i + 1] == '*') {
        size_t k = input_.find("*/", i + 2);
        if (k == std::string::npos) {
          Token t;
          t.kind = Token::Kind::Invalid;
          t.start = i;
          t.end = n;
          t.need_more = true;
          t.text = "unterminated comment";
          out.push_back(std::move(t));
          return out;
        }
        i = k + 2;
        continue;
      }
      if (c == '\'') {
        Token t;
        t.kind = Token::Kind::String;
        t.start = i;
        ++i;
        std::string value;
        bool closed = false;
        while (i < n) {
          char ch = input_[i];
          if (ch == '\'') {
            if (i + 1 < n && input_[i + 1] == '\'') {
              value.push_back('\'');
              i += 2;
              continue;
            }
            ++i;
            closed = true;
            break;
          }
          if (ch == '\\' && i + 1 < n) {
            char e = input_[i + 1];
            i += 2;
            switch (e) {
              case 'n': value.push_back('\n'); break;
              case 't': value.push_back('\t'); break;
              case 'r': value.push_back('\r'); break;
              case '0': value.push_back('\0'); break;
              case '\\': value.push_back('\\'); break;
              case '\'': value.push_back('\''); break;
              case '"': value.push_back('"'); break;
              default: value.push_back(e); break;
            }
            continue;
          }
          value.push_back(ch);
          ++i;
        }
        if (!closed) {
          t.kind = Token::Kind::Invalid;
          t.need_more = true;
          t.text = "unterminated string";
          t.end = n;
          out.push_back(std::move(t));
          return out;
        }
        t.text = std::move(value);
        t.end = i;
        out.push_back(std::move(t));
        continue;
      }
      if (c == '`') {
        Token t;
        t.kind = Token::Kind::Ident;
        t.start = i;
        ++i;
        std::string value;
        bool closed = false;
        while (i < n) {
          char ch = input_[i];
          if (ch == '`') {
            if (i + 1 < n && input_[i + 1] == '`') {
              value.push_back('`');
              i += 2;
              continue;
            }
            ++i;
            closed = true;
            break;
          }
          value.push_back(ch);
          ++i;
        }
        if (!closed) {
          t.kind = Token::Kind::Invalid;
          t.need_more = true;
          t.text = "unterminated identifier";
          t.end = n;
          out.push_back(std::move(t));
          return out;
        }
        t.text = std::move(value);
        t.end = i;
        out.push_back(std::move(t));
        continue;
      }
      if (std::isdigit(c)) {
        Token t;
        t.kind = Token::Kind::Number;
        t.start = i;
        while (i < n && std::isdigit(static_cast<unsigned char>(input_[i]))) ++i;
        // Capture decimal/exponent fragments so the parser can reject them
        // with a clear error instead of splitting unexpectedly.
        if (i < n && (input_[i] == '.' || input_[i] == 'e' || input_[i] == 'E')) {
          if (input_[i] == '.') {
            ++i;
            while (i < n && std::isdigit(static_cast<unsigned char>(input_[i]))) ++i;
          }
          if (i < n && (input_[i] == 'e' || input_[i] == 'E')) {
            ++i;
            if (i < n && (input_[i] == '+' || input_[i] == '-')) ++i;
            while (i < n && std::isdigit(static_cast<unsigned char>(input_[i]))) ++i;
          }
        }
        t.text = input_.substr(t.start, i - t.start);
        t.end = i;
        out.push_back(std::move(t));
        continue;
      }
      if (std::isalpha(c) || c == '_') {
        Token t;
        t.kind = Token::Kind::Ident;
        t.start = i;
        while (i < n) {
          unsigned char d = static_cast<unsigned char>(input_[i]);
          if (!std::isalnum(d) && d != '_') break;
          ++i;
        }
        t.text = input_.substr(t.start, i - t.start);
        t.end = i;
        out.push_back(std::move(t));
        continue;
      }
      // Punctuation
      Token t;
      t.kind = Token::Kind::Punct;
      t.start = i;
      t.punct = input_[i];
      t.end = i + 1;
      if ((input_[i] == '<' || input_[i] == '>' || input_[i] == '!') &&
          i + 1 < n && input_[i + 1] == '=') {
        t.punct = 0;
        t.text = input_.substr(i, 2);
        t.end = i + 2;
        i += 2;
        out.push_back(std::move(t));
        continue;
      }
      if (input_[i] == '<' && i + 1 < n && input_[i + 1] == '>') {
        t.punct = 0;
        t.text = "!=";
        t.end = i + 2;
        i += 2;
        out.push_back(std::move(t));
        continue;
      }
      ++i;
      out.push_back(std::move(t));
    }
    Token end;
    end.kind = Token::Kind::End;
    end.start = end.end = n;
    out.push_back(std::move(end));
    return out;
  }

 private:
  const std::string& input_;
};

class Parser {
 public:
  explicit Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {}

  Statement parse() {
    skipSemicolons();
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    Statement stmt;
    if (acceptKeyword("CREATE")) {
      if (acceptKeyword("TABLE")) {
        stmt.kind = Statement::Kind::CreateTable;
        stmt.create_table = parseCreateTable();
      } else if (acceptKeyword("VECTOR")) {
        expectKeyword("INDEX");
        stmt.kind = Statement::Kind::CreateIndex;
        stmt.create_index = parseCreateIndex();
      } else {
        fail(Err::kSyntax, "expected TABLE or VECTOR after CREATE");
      }
    } else if (acceptKeyword("DROP")) {
      expectKeyword("TABLE");
      stmt.kind = Statement::Kind::DropTable;
      stmt.drop_table = parseDropTable();
    } else if (acceptKeyword("TRUNCATE")) {
      acceptKeyword("TABLE");
      stmt.kind = Statement::Kind::TruncateTable;
      stmt.truncate_table = expectIdent();
      expectEnd();
    } else if (acceptKeyword("SET")) {
      stmt.kind = Statement::Kind::Set;
      stmt.set = parseSet();
    } else if (acceptKeyword("USE")) {
      stmt.kind = Statement::Kind::Use;
      stmt.use_db = expectIdent();
      expectEnd();
    } else if (acceptKeyword("INSERT")) {
      expectKeyword("INTO");
      stmt.kind = Statement::Kind::Insert;
      stmt.insert = parseInsert();
    } else if (acceptKeyword("SELECT")) {
      stmt.kind = Statement::Kind::Select;
      stmt.select = parseSelect();
    } else if (acceptKeyword("UPDATE")) {
      stmt.kind = Statement::Kind::Update;
      stmt.update = parseUpdate();
    } else if (acceptKeyword("DELETE")) {
      expectKeyword("FROM");
      stmt.kind = Statement::Kind::Delete;
      stmt.del = parseDelete();
    } else if (acceptKeyword("BEGIN")) {
      stmt.kind = Statement::Kind::Begin;
      expectEnd();
    } else if (acceptKeyword("COMMIT")) {
      stmt.kind = Statement::Kind::Commit;
      expectEnd();
    } else if (acceptKeyword("ROLLBACK")) {
      stmt.kind = Statement::Kind::Rollback;
      expectEnd();
    } else if (acceptKeyword("EXIT") || acceptKeyword("QUIT")) {
      stmt.kind = Statement::Kind::Exit;
      expectEnd();
    } else {
      fail(Err::kUnsupported, "unsupported or unknown statement");
    }
    return stmt;
  }

  size_t consumed() const { return consumed_; }

 private:
  const Token& peek(size_t k = 0) const {
    size_t idx = pos_ + k;
    if (idx >= tokens_.size()) return tokens_.back();
    return tokens_[idx];
  }
  const Token& next() {
    const Token& t = peek();
    if (t.kind != Token::Kind::End) ++pos_;
    last_end_ = t.end;
    return t;
  }
  bool acceptPunct(char c) {
    if (peek().kind == Token::Kind::Punct && peek().punct == c) {
      next();
      return true;
    }
    return false;
  }
  bool acceptPunctText(const char* text) {
    if (peek().kind == Token::Kind::Punct && peek().text == text) {
      next();
      return true;
    }
    return false;
  }
  void expectPunct(char c) {
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    if (!acceptPunct(c)) {
      fail(Err::kSyntax, std::string("expected '") + c + "'");
    }
  }
  bool acceptKeyword(const char* kw) {
    if (peek().kind == Token::Kind::Ident && iequals(peek().text, kw)) {
      next();
      return true;
    }
    return false;
  }
  void expectKeyword(const char* kw) {
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    if (!acceptKeyword(kw)) {
      fail(Err::kSyntax, std::string("expected ") + kw);
    }
  }
  std::string expectIdent() {
    if (peek().kind != Token::Kind::Ident) {
      if (peek().kind == Token::Kind::End) throw NeedMore{};
      fail(Err::kSyntax, "expected identifier");
    }
    return next().text;
  }
  uint64_t expectUInt() {
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    bool neg = false;
    if (peek().kind == Token::Kind::Punct && peek().punct == '-') {
      neg = true;
      next();
    }
    if (peek().kind != Token::Kind::Number) {
      fail(Err::kSyntax, "expected unsigned integer");
    }
    std::string text = next().text;
    if (text.find_first_of(".eE") != std::string::npos) {
      fail(Err::kInvalidValue, "expected integer, got " + text);
    }
    int64_t v = 0;
    std::string err;
    if (!parseBigInt(text, v, err) || v < 0 || neg) {
      fail(Err::kInvalidValue, err.empty() ? "integer out of range" : err);
    }
    return static_cast<uint64_t>(v);
  }
  Value parseLiteral() {
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    if (peek().kind == Token::Kind::Punct && peek().punct == '-') {
      next();
      if (peek().kind != Token::Kind::Number) fail(Err::kSyntax, "expected number");
      std::string text = "-" + next().text;
      int64_t v = 0;
      std::string err;
      if (!parseBigInt(text, v, err)) fail(Err::kInvalidValue, err);
      return Value::MakeInt(v);
    }
    if (peek().kind == Token::Kind::Number) {
      std::string text = next().text;
      if (text.find_first_of(".eE") != std::string::npos) {
        fail(Err::kInvalidValue, "BIGINT literal must be an integer: " + text);
      }
      int64_t v = 0;
      std::string err;
      if (!parseBigInt(text, v, err)) fail(Err::kInvalidValue, err);
      return Value::MakeInt(v);
    }
    if (peek().kind == Token::Kind::String) {
      return Value::MakeStr(next().text);
    }
    fail(Err::kSyntax, "expected literal value");
    return Value{};
  }
  void skipSemicolons() {
    while (peek().kind == Token::Kind::Punct && peek().punct == ';') next();
  }
  void expectEnd() {
    if (peek().kind == Token::Kind::Punct && peek().punct == ';') {
      const Token& t = next();
      consumed_ = t.end;
      return;
    }
    if (peek().kind == Token::Kind::End) {
      consumed_ = last_end_;
      return;
    }
    if (peek().kind == Token::Kind::Invalid) {
      if (peek().need_more) throw NeedMore{};
      fail(Err::kSyntax, peek().text);
    }
    fail(Err::kSyntax, "unexpected token after statement: " + peek().text);
  }
  [[noreturn]] void fail(int code, const std::string& msg) {
    throw ParseError(Status::Error(code, msg));
  }

  SetStmt parseSet() {
    if (acceptKeyword("SESSION") || acceptKeyword("LOCAL") || acceptKeyword("GLOBAL")) {
      // session/global setting
    }
    if (acceptPunct('@')) {
      expectPunct('@');
    }
    SetStmt stmt;
    stmt.key = expectIdent();
    // MySQL accepts scope prefixes such as @@session.autocommit.
    while (acceptPunct('.')) {
      stmt.key += ".";
      stmt.key += expectIdent();
    }
    if (!acceptPunct('=')) {
      // Forms such as SET NAMES utf8mb4 / SET TRANSACTION ... are accepted
      // as session no-ops; consume the rest of this statement.
      while (peek().kind != Token::Kind::End &&
             !(peek().kind == Token::Kind::Punct && peek().punct == ';')) {
        next();
      }
      expectEnd();
      return stmt;
    }
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    if (peek().kind == Token::Kind::String) {
      stmt.value = next().text;
    } else if (peek().kind == Token::Kind::Number) {
      stmt.value = next().text;
    } else if (peek().kind == Token::Kind::Punct && peek().punct == '-') {
      next();
      if (peek().kind != Token::Kind::Number) fail(Err::kSyntax, "expected number");
      stmt.value = "-" + next().text;
    } else {
      stmt.value = expectIdent();
    }
    expectEnd();
    return stmt;
  }

  DropTableStmt parseDropTable() {
    DropTableStmt stmt;
    if (acceptKeyword("IF")) {
      expectKeyword("EXISTS");
      stmt.if_exists = true;
    }
    stmt.table = expectIdent();
    expectEnd();
    return stmt;
  }

  CreateTableStmt parseCreateTable() {
    CreateTableStmt stmt;
    if (acceptKeyword("IF")) {
      expectKeyword("NOT");
      expectKeyword("EXISTS");
      stmt.if_not_exists = true;
    }
    stmt.table = expectIdent();
    expectPunct('(');
    bool saw_primary_key = false;
    while (true) {
      if (acceptKeyword("PRIMARY")) {
        expectKeyword("KEY");
        expectPunct('(');
        std::string pk_col = expectIdent();
        expectPunct(')');
        for (size_t i = 0; i < stmt.columns.size(); ++i) {
          if (iequals(stmt.columns[i].name, pk_col)) {
            if (saw_primary_key) fail(Err::kSyntax, "multiple primary keys");
            stmt.pk_index = static_cast<int>(i);
            saw_primary_key = true;
          }
        }
        if (!saw_primary_key) fail(Err::kColumnNotFound, "unknown primary key column: " + pk_col);
        expectPunct(')');
        break;
      }
      Column col;
      col.name = expectIdent();
      if (acceptKeyword("BIGINT")) {
        col.type = ColumnType::BigInt;
      } else if (acceptKeyword("VARCHAR")) {
        col.type = ColumnType::Varchar;
        expectPunct('(');
        col.length = static_cast<uint32_t>(expectUInt());
        expectPunct(')');
        if (col.length == 0) fail(Err::kInvalidValue, "VARCHAR length must be positive");
      } else if (acceptKeyword("VECTOR")) {
        col.type = ColumnType::Vector;
        expectPunct('(');
        col.length = static_cast<uint32_t>(expectUInt());
        expectPunct(')');
        if (col.length == 0) fail(Err::kInvalidValue, "VECTOR dimension must be positive");
      } else {
        fail(Err::kUnsupported, "unsupported column type for " + col.name);
      }
      for (const auto& existing : stmt.columns) {
        if (iequals(existing.name, col.name)) {
          fail(Err::kSyntax, "duplicate column: " + col.name);
        }
      }
      if (acceptKeyword("PRIMARY")) {
        expectKeyword("KEY");
        if (saw_primary_key) fail(Err::kSyntax, "multiple primary keys");
        stmt.pk_index = static_cast<int>(stmt.columns.size());
        saw_primary_key = true;
      }
      stmt.columns.push_back(std::move(col));
      if (acceptPunct(',')) continue;
      expectPunct(')');
      break;
    }
    if (stmt.columns.empty()) fail(Err::kSyntax, "table must have at least one column");
    if (stmt.pk_index < 0) fail(Err::kSyntax, "table must have one primary key");
    if (stmt.columns[stmt.pk_index].type != ColumnType::BigInt) {
      fail(Err::kUnsupported, "primary key must be BIGINT");
    }
    int vector_count = 0;
    for (const auto& col : stmt.columns) {
      if (col.type == ColumnType::Vector) ++vector_count;
    }
    if (vector_count > 1) fail(Err::kUnsupported, "at most one VECTOR column per table");
    expectEnd();
    return stmt;
  }

  CreateIndexStmt parseCreateIndex() {
    CreateIndexStmt stmt;
    stmt.name = expectIdent();
    expectKeyword("ON");
    stmt.table = expectIdent();
    expectPunct('(');
    stmt.column = expectIdent();
    expectPunct(')');
    expectEnd();
    return stmt;
  }

  InsertStmt parseInsert() {
    InsertStmt stmt;
    stmt.table = expectIdent();
    if (acceptPunct('(')) {
      while (true) {
        stmt.columns.push_back(expectIdent());
        if (acceptPunct(',')) continue;
        expectPunct(')');
        break;
      }
    }
    expectKeyword("VALUES");
    while (true) {
      expectPunct('(');
      std::vector<Value> row;
      while (true) {
        row.push_back(parseLiteral());
        if (acceptPunct(',')) continue;
        expectPunct(')');
        break;
      }
      stmt.rows.push_back(std::move(row));
      if (acceptPunct(',')) continue;
      break;
    }
    expectEnd();
    return stmt;
  }

  std::vector<Condition> parseWhere() {
    std::vector<Condition> out;
    while (true) {
      Condition c;
      c.column = expectIdent();
      c.op = parseOp();
      c.value = parseLiteral();
      out.push_back(std::move(c));
      if (acceptKeyword("AND")) continue;
      break;
    }
    return out;
  }

  std::string parseOp() {
    if (peek().kind == Token::Kind::End) throw NeedMore{};
    if (peek().kind == Token::Kind::Punct) {
      std::string t = peek().text;
      if (t.empty() && peek().punct != 0) t = std::string(1, peek().punct);
      if (t == "=" || t == "<" || t == "<=" || t == ">" || t == ">=" ||
          t == "!=") {
        next();
        return t;
      }
    }
    fail(Err::kSyntax, "expected comparison operator");
    return {};
  }

  SelectStmt parseSelect() {
    SelectStmt stmt;
    while (true) {
      SelectItem item;
      if (acceptPunct('*')) {
        item.kind = SelectItem::Kind::Star;
        item.text = "*";
      } else if (peek().kind == Token::Kind::Ident &&
                 iequals(peek().text, "L2_DISTANCE")) {
        next();
        expectPunct('(');
        item.kind = SelectItem::Kind::Distance;
        item.column = expectIdent();
        expectPunct(',');
        if (peek().kind == Token::Kind::End) throw NeedMore{};
        if (peek().kind != Token::Kind::String) {
          fail(Err::kSyntax, "L2_DISTANCE requires a vector literal string");
        }
        item.vector_text = next().text;
        expectPunct(')');
        item.text = "L2_DISTANCE(" + item.column + ", '" + item.vector_text + "')";
      } else {
        item.kind = SelectItem::Kind::Column;
        item.column = expectIdent();
        item.text = item.column;
      }
      if (acceptKeyword("AS")) {
        item.alias = expectIdent();
      }
      stmt.items.push_back(std::move(item));
      if (acceptPunct(',')) continue;
      break;
    }
    expectKeyword("FROM");
    stmt.table = expectIdent();
    if (acceptKeyword("WHERE")) stmt.where = parseWhere();
    if (acceptKeyword("ORDER")) {
      expectKeyword("BY");
      while (true) {
        OrderItem item;
        item.column = expectIdent();
        if (acceptKeyword("ASC")) {
          item.desc = false;
        } else if (acceptKeyword("DESC")) {
          item.desc = true;
        }
        stmt.order.push_back(std::move(item));
        if (acceptPunct(',')) continue;
        break;
      }
    }
    if (acceptKeyword("LIMIT")) {
      stmt.has_limit = true;
      stmt.limit = expectUInt();
    }
    expectEnd();
    return stmt;
  }

  UpdateStmt parseUpdate() {
    UpdateStmt stmt;
    stmt.table = expectIdent();
    expectKeyword("SET");
    while (true) {
      std::string col = expectIdent();
      expectPunct('=');
      Value v = parseLiteral();
      stmt.sets.emplace_back(std::move(col), std::move(v));
      if (acceptPunct(',')) continue;
      break;
    }
    if (acceptKeyword("WHERE")) stmt.where = parseWhere();
    expectEnd();
    return stmt;
  }

  DeleteStmt parseDelete() {
    DeleteStmt stmt;
    stmt.table = expectIdent();
    if (acceptKeyword("WHERE")) stmt.where = parseWhere();
    expectEnd();
    return stmt;
  }

  std::vector<Token> tokens_;
  size_t pos_ = 0;
  size_t last_end_ = 0;
  size_t consumed_ = 0;
};

}  // namespace

ParseResult parseStatement(const std::string& input) {
  Lexer lexer(input);
  std::vector<Token> tokens = lexer.run();
  for (const auto& t : tokens) {
    if (t.kind == Token::Kind::Invalid) {
      if (t.need_more) return ParseResult::MakeNeedMore();
      return ParseResult::MakeError(Status::Error(Err::kSyntax, t.text));
    }
  }
  bool only_empty = true;
  for (const auto& t : tokens) {
    if (t.kind == Token::Kind::End) continue;
    if (t.kind == Token::Kind::Punct && t.punct == ';') continue;
    only_empty = false;
    break;
  }
  if (only_empty) return ParseResult::MakeEmpty();
  Parser parser(std::move(tokens));
  try {
    Statement stmt = parser.parse();
    return ParseResult::MakeOk(std::move(stmt), parser.consumed());
  } catch (const NeedMore&) {
    return ParseResult::MakeNeedMore();
  } catch (const ParseError& e) {
    return ParseResult::MakeError(e.status);
  }
}

}  // namespace mvdb
