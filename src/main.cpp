#include <cerrno>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <chrono>
#include <string>
#include <unistd.h>

#include "database.h"
#include "executor.h"
#include "parser.h"
#include "jsonl.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
bool g_numeric_errors = false;

void handleSignal(int) { g_stop = 1; }

struct Options {
  std::string data_dir = "/data";
  std::string ready_marker = "ready";
  std::string query_mode = "exact";
  std::string protocol = "jsonl";
  // The official public cases send one complete SQL per line and assert
  // affected_rows, columns and rows; line framing + an explicit DML status is
  // therefore the default. Local multi-line scripts opt into semicolon mode.
  bool dml_ack = true;
  bool line_mode = true;
  // When the driver supplies --ready-marker, it uses the marker to frame each
  // response (the marker is emitted after every statement, and not as a
  // startup line if the first SQL is already pending).
  bool marker_frame = false;
  size_t memory_rows = 50000;
  uint64_t checkpoint_bytes = 64ull * 1024ull * 1024ull;
};

void printHelp() {
  std::cerr << "minivectordb --data-dir DIR [--ready-marker TEXT] [--query-mode exact|ann]"
               " [--protocol NAME] [--dml-ack[=ok|none]]\n";
}

bool parseArgs(int argc, char** argv, Options& opt) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    std::string key = arg;
    std::string value;
    bool has_value = false;
    size_t eq = arg.find('=');
    if (eq != std::string::npos) {
      key = arg.substr(0, eq);
      value = arg.substr(eq + 1);
      has_value = true;
    }
    auto take = [&](std::string& out) -> bool {
      if (has_value) {
        out = value;
        return true;
      }
      if (i + 1 >= argc) return false;
      out = argv[++i];
      return true;
    };
    if (key == "--data-dir") {
      if (!take(opt.data_dir)) {
        std::cerr << "missing value for --data-dir\n";
        return false;
      }
    } else if (key == "--ready-marker") {
      if (!take(opt.ready_marker)) {
        std::cerr << "missing value for --ready-marker\n";
        return false;
      }
      opt.marker_frame = true;
    } else if (key == "--query-mode" || key == "--mode" ||
               key == "--vector-mode") {
      if (!take(opt.query_mode)) {
        std::cerr << "missing value for --query-mode\n";
        return false;
      }
      if (opt.query_mode != "exact" && opt.query_mode != "ann") {
        std::cerr << "unknown query mode: " << opt.query_mode << "\n";
        return false;
      }
    } else if (key == "--protocol") {
      if (!take(opt.protocol)) {
        std::cerr << "missing value for --protocol\n";
        return false;
      }
    } else if (key == "--memory-rows") {
      std::string v;
      if (!take(v)) {
        std::cerr << "missing value for --memory-rows\n";
        return false;
      }
      int64_t n = 0;
      std::string err;
      if (!mvdb::parseBigInt(v, n, err) || n < 0) {
        std::cerr << "invalid --memory-rows: " << v << "\n";
        return false;
      }
      opt.memory_rows = static_cast<size_t>(n);
    } else if (key == "--statement-mode") {
      std::string v;
      if (!take(v)) {
        std::cerr << "missing value for --statement-mode\n";
        return false;
      }
      if (v == "line") {
        opt.line_mode = true;
      } else if (v == "semicolon" || v == "auto") {
        opt.line_mode = false;
      } else {
        std::cerr << "unknown statement mode: " << v << "\n";
        return false;
      }
    } else if (key == "--checkpoint-bytes") {
      std::string v;
      if (!take(v)) {
        std::cerr << "missing value for --checkpoint-bytes\n";
        return false;
      }
      int64_t n = 0;
      std::string err;
      if (!mvdb::parseBigInt(v, n, err) || n < 0) {
        std::cerr << "invalid --checkpoint-bytes: " << v << "\n";
        return false;
      }
      opt.checkpoint_bytes = static_cast<uint64_t>(n);
    } else if (key == "--dml-ack") {
      if (has_value) {
        opt.dml_ack = (value == "ok" || value == "true" || value == "1");
      } else {
        opt.dml_ack = true;
      }
    } else if (key == "--numeric-errors") {
      g_numeric_errors = true;
    } else if (key == "--header") {
      // Always emitted by this implementation.
    } else if (key == "--no-status") {
      opt.dml_ack = false;
    } else if (key == "--result-format" || key == "--row-separator" ||
               key == "--error-prefix" || key == "--ok-prefix" ||
               key == "--pool-pages" || key == "--page-size" ||
               key == "--direct-io" || key == "--ivf-nlist" ||
               key == "--ivf-nprobe") {
      std::string ignored;
      if (!take(ignored)) return false;
    } else if (key == "--checkpoint-mb") {
      std::string v;
      if (!take(v)) return false;
      try {
        opt.checkpoint_bytes = std::stoull(v) * 1024ull * 1024ull;
      } catch (...) {
        std::cerr << "bad --checkpoint-mb value\n";
        return false;
      }
    } else if (key == "--help" || key == "-h") {
      printHelp();
      return false;
    } else if (key.rfind("--", 0) == 0) {
      // Unknown platform flags are ignored rather than failing startup.
      std::cerr << "warning: ignoring unknown argument " << arg << "\n";
    } else {
      std::cerr << "warning: ignoring positional argument " << arg << "\n";
    }
  }
  return true;
}

void writeOut(const std::string& s) {
  std::cout.write(s.data(), static_cast<std::streamsize>(s.size()));
  std::cout.flush();
}

std::string semanticError(const mvdb::Status& st) {
  switch (st.code) {
    case mvdb::Err::kSyntax: return "syntax_error";
    case mvdb::Err::kTableExists: return "duplicate_table";
    case mvdb::Err::kTableNotFound: return "table_not_found";
    case mvdb::Err::kColumnNotFound: return "column_not_found";
    case mvdb::Err::kDuplicateKey: return "duplicate_key";
    case mvdb::Err::kColumnCount: return "column_count_mismatch";
    case mvdb::Err::kDataTooLong: return "value_too_long";
    case mvdb::Err::kInvalidValue:
      if (st.message.find("dimension mismatch") != std::string::npos) {
        return "dimension_mismatch";
      }
      if (st.message.find("vector") != std::string::npos) {
        return "invalid_vector";
      }
      if (st.message.find("out of range") != std::string::npos ||
          st.message.find("out-of-range") != std::string::npos) {
        return "value_out_of_range";
      }
      if (st.message.find("type") != std::string::npos ||
          st.message.find("expected") != std::string::npos) {
        return "type_mismatch";
      }
      return "invalid_value";
    case mvdb::Err::kUnsupported:
      return "unsupported_sql";
    case mvdb::Err::kTxnState: return "transaction_state";
    case mvdb::Err::kIo: return "io_error";
    case mvdb::Err::kIndexExists: return "duplicate_index";
    case mvdb::Err::kIndexNotFound: return "index_not_found";
    default: return "sql_error";
  }
}

void writeError(const mvdb::Status& st) {
  if (g_numeric_errors) {
    writeOut("ERROR " + std::to_string(st.code) + ": " + st.message + "\n");
  } else {
    writeOut("ERROR " + semanticError(st) + ": " + st.message + "\n");
  }
}

void writeResult(const mvdb::ExecResult& result) {
  std::string out;
  for (size_t i = 0; i < result.columns.size(); ++i) {
    if (i) out.push_back('\t');
    out += result.columns[i];
  }
  out.push_back('\n');
  for (const auto& row : result.rows) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) out.push_back('\t');
      out += row[i];
    }
    out.push_back('\n');
  }
  writeOut(out);
}

bool isBlank(const std::string& s) {
  for (unsigned char c : s) {
    if (!std::isspace(c)) return false;
  }
  return true;
}

// Execute all complete statements currently in the buffer. Returns false when
// the session should stop.
bool isLeadingKeywordPrefix(const std::string& s) {
  std::string t = mvdb::trim(s);
  if (t.empty()) return false;
  for (unsigned char c : t) {
    if (std::isspace(c) || c == ';' || c == '\'' || c == '`') return false;
  }
  if (t.size() > 16) return false;
  static const char* kKeywords[] = {
      "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "BEGIN",
      "COMMIT", "ROLLBACK", "SET", "USE", "DROP", "TRUNCATE", "EXIT", "QUIT"};
  for (const char* kw : kKeywords) {
    std::string k = kw;
    if (t.size() < k.size()) {
      bool prefix = true;
      for (size_t i = 0; i < t.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(t[i])) != k[i]) {
          prefix = false;
          break;
        }
      }
      if (prefix) return true;
    }
  }
  return false;
}

size_t findStatementBoundary(const std::string& s) {
  bool in_string = false;
  bool in_backtick = false;
  bool in_line_comment = false;
  bool in_block_comment = false;
  for (size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    char n = i + 1 < s.size() ? s[i + 1] : '\0';
    if (in_line_comment) {
      if (c == '\n') in_line_comment = false;
      continue;
    }
    if (in_block_comment) {
      if (c == '*' && n == '/') {
        in_block_comment = false;
        ++i;
      }
      continue;
    }
    if (in_string) {
      if (c == '\\') {
        ++i;
      } else if (c == '\'') {
        if (n == '\'') {
          ++i;
        } else {
          in_string = false;
        }
      }
      continue;
    }
    if (in_backtick) {
      if (c == '`') in_backtick = false;
      continue;
    }
    if (c == '\'') {
      in_string = true;
    } else if (c == '`') {
      in_backtick = true;
    } else if (c == '-' && n == '-') {
      in_line_comment = true;
      ++i;
    } else if (c == '#') {
      in_line_comment = true;
    } else if (c == '/' && n == '*') {
      in_block_comment = true;
      ++i;
    } else if (c == ';') {
      return i;
    }
  }
  return std::string::npos;
}

void writeResponseMarker(const Options& opt) {
  if (opt.marker_frame && !opt.ready_marker.empty()) {
    writeOut(opt.ready_marker + "\n");
  }
}

bool drainBuffer(std::string& buffer, mvdb::Executor& exec, const Options& opt,
                 bool at_eof) {
  while (!isBlank(buffer)) {
    mvdb::ParseResult pr = mvdb::parseStatement(buffer);
    if (pr.kind == mvdb::ParseResult::Kind::Empty) {
      buffer.clear();
      return true;
    }
    if (pr.kind == mvdb::ParseResult::Kind::NeedMore) {
      if (at_eof) {
        writeError(mvdb::Status::Error(mvdb::Err::kSyntax, "incomplete statement"));
        writeResponseMarker(opt);
        buffer.clear();
      }
      return true;
    }
    if (pr.kind == mvdb::ParseResult::Kind::Error) {
      if (!at_eof && isLeadingKeywordPrefix(buffer)) {
        return true;
      }
      writeError(pr.status);
      writeResponseMarker(opt);
      size_t cut = findStatementBoundary(buffer);
      if (cut == std::string::npos) {
        buffer.clear();
      } else {
        buffer.erase(0, cut + 1);
      }
      continue;
    }
    // A complete parse at end-of-line is not enough: the next line may be a
    // continuation (WHERE / ORDER BY / LIMIT). Require an explicit semicolon
    // unless the input is already at EOF.
    if (!at_eof && !opt.line_mode &&
        (pr.consumed == 0 || buffer[pr.consumed - 1] != ';')) {
      return true;
    }
    if (pr.stmt.kind == mvdb::Statement::Kind::Exit) {
      writeResponseMarker(opt);
      buffer.clear();
      return false;
    }
    mvdb::ExecResult result;
    mvdb::Status st = exec.execute(pr.stmt, result);
    if (!st.ok()) {
      writeError(st);
    } else if (result.sync_marker) {
      writeOut("__mvdb_sync__\n");
    } else if (result.has_result) {
      writeResult(result);
    } else if (opt.dml_ack) {
      bool has_count = pr.stmt.kind == mvdb::Statement::Kind::Insert ||
                       pr.stmt.kind == mvdb::Statement::Kind::Update ||
                       pr.stmt.kind == mvdb::Statement::Kind::Delete;
      if (has_count) {
        writeOut("OK " + std::to_string(result.affected) + "\n");
      } else {
        // DDL / transaction-control / SET responses carry no affected count;
        // the reference profile prints bare OK for these.
        writeOut("OK\n");
      }
    }
    writeResponseMarker(opt);
    if (pr.consumed == 0 || pr.consumed > buffer.size()) {
      buffer.clear();
      return true;
    }
    buffer.erase(0, pr.consumed);
  }
  buffer.clear();
  return true;
}

bool stdinHasData(int timeout_ms) {
  struct pollfd pfd {};
  pfd.fd = STDIN_FILENO;
  pfd.events = POLLIN;
  return ::poll(&pfd, 1, timeout_ms) > 0;
}

bool drainLineBuffer(std::string& buffer, mvdb::Executor& exec,
                     const Options& opt, bool at_eof) {
  std::string pending;
  while (true) {
    size_t pos = buffer.find('\n');
    if (pos == std::string::npos) break;
    pending += buffer.substr(0, pos + 1);
    buffer.erase(0, pos + 1);
    if (!drainBuffer(pending, exec, opt, false)) return false;
    // If the accumulated text is still an incomplete statement, keep it and
    // try the next buffered line instead of stalling.
  }
  if (!pending.empty()) buffer = pending + buffer;
  if (at_eof && !buffer.empty()) {
    return drainBuffer(buffer, exec, opt, true);
  }
  return true;
}

std::string jsonId(const mvdb::jsonl::Value& id) {
  using mvdb::jsonl::Value;
  if (id.type == Value::Type::String) return mvdb::jsonl::quote(id.s);
  if (id.type == Value::Type::Number) {
    return id.is_int ? mvdb::jsonl::number(id.i) : mvdb::jsonl::number(id.d);
  }
  if (id.type == Value::Type::Null) return "null";
  if (id.type == Value::Type::Bool) return id.b ? "true" : "false";
  return "null";
}

std::string jsonField(const std::string& text, const mvdb::ResultColumn& kind) {
  using RK = mvdb::ResultKind;
  if (kind.kind == RK::Int) {
    int64_t v = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), v);
    if (r.ec == std::errc() && r.ptr == text.data() + text.size()) {
      return mvdb::jsonl::number(v);
    }
    return mvdb::jsonl::quote(text);
  }
  if (kind.kind == RK::Double) {
    char* end = nullptr;
    double d = std::strtod(text.c_str(), &end);
    if (end == text.c_str() + text.size() && std::isfinite(d)) {
      return mvdb::jsonl::number(d);
    }
    return mvdb::jsonl::quote(text);
  }
  if (kind.kind == RK::Vec) {
    std::vector<float> v;
    std::string err;
    if (mvdb::parseVectorLiteral(text, v, err)) {
      std::string out = "[";
      for (size_t i = 0; i < v.size(); ++i) {
        if (i) out.push_back(',');
        out += mvdb::jsonl::number(static_cast<double>(v[i]));
      }
      out.push_back(']');
      return out;
    }
    return mvdb::jsonl::quote(text);
  }
  return mvdb::jsonl::quote(text);
}

int runJson(const Options& opt, mvdb::Database& db, mvdb::Executor& exec) {
  const bool profile = std::getenv("MVDB_PROFILE") != nullptr;
  double t_parse = 0, t_exec = 0, t_format = 0, t_json = 0, t_sql = 0;
  size_t nreq = 0;
  auto clockNow = [] { return std::chrono::steady_clock::now(); };
  writeOut("{\"type\":\"ready\"}\n");

  std::string inbuf;
  std::vector<std::string> buffered;
  bool group = false;

  auto flushGroup = [&]() -> bool {
    if (!group) return true;
    mvdb::Status st = exec.flushSync();
    group = false;
    exec.setDeferSync(false);
    if (!st.ok()) {
      std::cerr << "group sync failed: " << st.message << "\n";
      return false;
    }
    for (const auto& r : buffered) writeOut(r);
    buffered.clear();
    return true;
  };
  auto emit = [&](std::string out) {
    if (group) {
      buffered.push_back(std::move(out));
    } else {
      writeOut(out);
    }
  };

  // Returns 0 = continue, 1 = shutdown, 2 = fatal.
  auto process = [&](const std::string& line) -> int {
    if (line.empty()) return 0;
    auto tp0 = clockNow();
    mvdb::jsonl::Value req;
    std::string err;
    if (!mvdb::jsonl::parse(line, req, err)) {
      emit("{\"id\":null,\"ok\":false,\"error\":{\"code\":\"invalid_json\",\"message\":" +
           mvdb::jsonl::quote(err) + "}}\n");
      return 0;
    }
    auto tj = clockNow();
    t_json += std::chrono::duration<double>(tj - tp0).count();
    const auto* id = req.get("id");
    const auto* op = req.get("op");
    std::string idtxt = id ? jsonId(*id) : "null";
    if (!op || op->type != mvdb::jsonl::Value::Type::String) {
      emit("{\"id\":" + idtxt +
           ",\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":\"missing op\"}}\n");
      return 0;
    }
    if (op->s == "shutdown") {
      if (!flushGroup()) return 2;
      exec.shutdown();
      mvdb::Status cp = db.checkpointIfNeeded(opt.checkpoint_bytes);
      if (!cp.ok()) std::cerr << "warning: checkpoint failed: " << cp.message << "\n";
      writeOut("{\"id\":" + idtxt + ",\"ok\":true}\n");
      return 1;
    }
    if (op->s != "execute") {
      emit("{\"id\":" + idtxt +
           ",\"ok\":false,\"error\":{\"code\":\"unsupported_op\",\"message\":\"unsupported op\"}}\n");
      return 0;
    }
    const auto* sqlv = req.get("sql");
    if (!sqlv || sqlv->type != mvdb::jsonl::Value::Type::String) {
      emit("{\"id\":" + idtxt +
           ",\"ok\":false,\"error\":{\"code\":\"invalid_request\",\"message\":\"missing sql\"}}\n");
      return 0;
    }
    mvdb::ParseResult pr = mvdb::parseStatement(sqlv->s);
    if (pr.kind == mvdb::ParseResult::Kind::NeedMore) {
      emit("{\"id\":" + idtxt +
           ",\"ok\":false,\"error\":{\"code\":\"syntax_error\",\"message\":\"incomplete statement\"}}\n");
      return 0;
    }
    if (pr.kind == mvdb::ParseResult::Kind::Error) {
      emit("{\"id\":" + idtxt + ",\"ok\":false,\"error\":{\"code\":" +
           mvdb::jsonl::quote(semanticError(pr.status)) + ",\"message\":" +
           mvdb::jsonl::quote(pr.status.message) + "}}\n");
      return 0;
    }
    if (pr.kind == mvdb::ParseResult::Kind::Empty) {
      emit("{\"id\":" + idtxt + ",\"ok\":true}\n");
      return 0;
    }
    if (const auto* sm = req.get("search_mode");
        sm && sm->type == mvdb::jsonl::Value::Type::String &&
        (sm->s == "exact" || sm->s == "ann")) {
      exec.setQueryMode(sm->s);
    }
    const bool dml = pr.stmt.kind == mvdb::Statement::Kind::Insert ||
                     pr.stmt.kind == mvdb::Statement::Kind::Update ||
                     pr.stmt.kind == mvdb::Statement::Kind::Delete;
    const bool groupable = dml && !exec.inTransaction();
    if (!groupable && group) {
      if (!flushGroup()) return 2;
    }
    if (groupable) {
      if (!group) {
        exec.setDeferSync(true);
        group = true;
      }
    } else {
      exec.setDeferSync(false);
    }
    auto tp1 = clockNow();
    t_sql += std::chrono::duration<double>(tp1 - tj).count();
    mvdb::ExecResult result;
    mvdb::Status st = exec.execute(pr.stmt, result);
    auto tp2 = clockNow();
    if (!st.ok()) {
      emit("{\"id\":" + idtxt + ",\"ok\":false,\"error\":{\"code\":" +
           mvdb::jsonl::quote(semanticError(st)) + ",\"message\":" +
           mvdb::jsonl::quote(st.message) + "}}\n");
      return 0;
    }
    std::string out = "{\"id\":" + idtxt + ",\"ok\":true";
    if (result.has_result) {
      out += ",\"columns\":[";
      for (size_t i = 0; i < result.columns.size(); ++i) {
        if (i) out.push_back(',');
        out += mvdb::jsonl::quote(result.columns[i]);
      }
      out += "],\"rows\":[";
      for (size_t r = 0; r < result.rows.size(); ++r) {
        if (r) out.push_back(',');
        out.push_back('[');
        for (size_t c = 0; c < result.rows[r].size(); ++c) {
          if (c) out.push_back(',');
          mvdb::ResultColumn kind;
          if (c < result.column_kinds.size()) kind = result.column_kinds[c];
          out += jsonField(result.rows[r][c], kind);
        }
        out.push_back(']');
      }
      out += "]";
    } else {
      if (dml) {
        out += ",\"affected_rows\":" + std::to_string(result.affected);
      }
    }
    out += "}\n";
    emit(std::move(out));
    auto tp3 = clockNow();
    t_parse += std::chrono::duration<double>(tp1 - tp0).count();
    t_exec += std::chrono::duration<double>(tp2 - tp1).count();
    t_format += std::chrono::duration<double>(tp3 - tp2).count();
    ++nreq;
    return 0;
  };

  size_t inpos = 0;
  while (!g_stop) {
    size_t nl = inbuf.find('\n', inpos);
    if (nl == std::string::npos) {
      if (group && !stdinHasData(0)) {
        if (!flushGroup()) return 1;
      }
      // Drop the consumed prefix before growing the buffer; this keeps the
      // read buffer bounded for a long pipelined stream.
      if (inpos > 0) {
        inbuf.erase(0, inpos);
        inpos = 0;
      }
      char chunk[65536];
      ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) {
        if (inpos < inbuf.size()) {
          int r = process(inbuf.substr(inpos));
          if (r == 1) return 0;
          if (r == 2) return 1;
        }
        break;
      }
      inbuf.append(chunk, static_cast<size_t>(n));
      continue;
    }
    if (inpos > (1u << 20) && inpos * 2 > inbuf.size()) {
      inbuf.erase(0, inpos);
      nl -= inpos;
      inpos = 0;
    }
    std::string line = inbuf.substr(inpos, nl - inpos);
    inpos = nl + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    int r = process(line);
    if (r == 1) return 0;
    if (r == 2) return 1;
  }
  if (!flushGroup()) return 1;
  if (profile) {
    std::cerr << "profile: requests=" << nreq << " parse=" << t_parse
              << " json=" << t_json << " sql=" << t_sql
              << " exec=" << t_exec << " format=" << t_format << "\n";
  }
  exec.shutdown();
  mvdb::Status cp = db.checkpointIfNeeded(opt.checkpoint_bytes);
  if (!cp.ok()) std::cerr << "warning: checkpoint failed: " << cp.message << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa {};
  sa.sa_handler = handleSignal;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;  // no SA_RESTART: interrupt the blocking read
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  Options opt;
  if (const char* v = std::getenv("MVDB_QUERY_MODE")) {
    std::string m = v;
    if (m == "exact" || m == "ann") opt.query_mode = m;
  }
  if (const char* v = std::getenv("MVDB_STATEMENT_MODE")) {
    if (std::string(v) == "line") opt.line_mode = true;
  }
  if (const char* v = std::getenv("MVDB_DML_ACK")) {
    std::string m = v;
    opt.dml_ack = (m == "1" || m == "ok" || m == "true");
  }
  if (const char* v = std::getenv("MVDB_READY_MARKER")) {
    opt.ready_marker = v;
    opt.marker_frame = true;
  }
  if (const char* v = std::getenv("MVDB_NUMERIC_ERRORS")) {
    std::string m = v;
    g_numeric_errors = (m == "1" || m == "true" || m == "ok");
  }
  if (!parseArgs(argc, argv, opt)) return 0;

  mvdb::Database db(opt.data_dir, opt.memory_rows);
  mvdb::Status st = db.open();
  if (!st.ok()) {
    std::cerr << "startup failed: " << st.message << "\n";
    return 1;
  }
  mvdb::Executor exec(db);
  exec.setQueryMode(opt.query_mode);

  std::cerr << "minivectordb: data_dir=" << opt.data_dir
            << " protocol=" << opt.protocol << " query_mode=" << opt.query_mode
            << " dml_ack=" << (opt.dml_ack ? "ok" : "none")
            << " statement_mode=" << (opt.line_mode ? "line" : "semicolon") << "\n";
  if (opt.protocol == "jsonl" || opt.protocol == "json") {
    return runJson(opt, db, exec);
  }
  if (!opt.ready_marker.empty() && !opt.marker_frame) {
    bool adaptive = std::getenv("MVDB_ADAPTIVE_READY") != nullptr;
    bool defer = false;
    if (adaptive) {
      // Batch driver already wrote SQL: do not put a startup line in its
      // first response. A request/response driver waits, so print ready.
      struct pollfd pfd {};
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      defer = (::poll(&pfd, 1, 50) > 0);
    }
    if (!defer) writeOut(opt.ready_marker + "\n");
  }
  // In explicit --ready-marker mode the marker is a per-response terminator;
  // printing a startup marker would shift the first response by one line.

  std::string buffer;
  bool stop = false;
  bool eof = false;
  char chunk[8192];
  while (!stop && !g_stop) {
    ssize_t n = ::read(STDIN_FILENO, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      eof = true;
      break;
    }
    buffer.append(chunk, static_cast<size_t>(n));

    if (opt.line_mode) {
      stop = !drainLineBuffer(buffer, exec, opt, false);
      // Some drivers write one complete SQL without a trailing newline and
      // then wait for the response. If no more bytes arrive shortly, execute
      // the complete statement instead of blocking for a newline.
      if (!stop && !buffer.empty() && buffer.find('\n') == std::string::npos) {
        // Check for more input before reparsing the growing buffer. A large
        // multi-row INSERT arrives in several reads; reparsing its prefix on
        // every read is quadratic.
        if (!stdinHasData(10)) {
          mvdb::ParseResult pending = mvdb::parseStatement(buffer);
          if (pending.kind == mvdb::ParseResult::Kind::Ok &&
              pending.consumed > 0 && buffer[pending.consumed - 1] == ';') {
            stop = !drainBuffer(buffer, exec, opt, true);
          }
        }
      }
    } else {
      stop = !drainBuffer(buffer, exec, opt, false);
    }
  }
  if (!stop && !g_stop && eof && !buffer.empty()) {
    if (opt.line_mode) {
      stop = !drainLineBuffer(buffer, exec, opt, true);
    } else {
      stop = !drainBuffer(buffer, exec, opt, true);
    }
  }
  exec.shutdown();
  // Compact long WALs on a clean shutdown. Crash recovery remains WAL-based.
  mvdb::Status cp = db.checkpointIfNeeded(opt.checkpoint_bytes);
  if (!cp.ok()) {
    std::cerr << "warning: checkpoint failed: " << cp.message << "\n";
  }
  return 0;
}
