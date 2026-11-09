#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <string>
#include <unistd.h>

#include "database.h"
#include "executor.h"
#include "parser.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handleSignal(int) { g_stop = 1; }

struct Options {
  std::string data_dir = "/data";
  std::string ready_marker = "ready";
  std::string query_mode = "exact";
  std::string protocol = "mysqltest";
  // The official public cases send one complete SQL per line and assert
  // affected_rows, columns and rows; line framing + an explicit DML status is
  // therefore the default. Local multi-line scripts opt into semicolon mode.
  bool dml_ack = true;
  bool line_mode = true;
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
    } else if (key == "--query-mode" || key == "--mode") {
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

void writeError(const mvdb::Status& st) {
  writeOut("ERROR " + std::to_string(st.code) + ": " + st.message + "\n");
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
        buffer.clear();
      }
      return true;
    }
    if (pr.kind == mvdb::ParseResult::Kind::Error) {
      if (!at_eof && isLeadingKeywordPrefix(buffer)) {
        return true;
      }
      writeError(pr.status);
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
      writeOut("OK " + std::to_string(result.affected) + "\n");
    }
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
  }
  if (!parseArgs(argc, argv, opt)) return 0;

  mvdb::Database db(opt.data_dir, opt.memory_rows);
  mvdb::Status st = db.open();
  if (!st.ok()) {
    std::cerr << "startup failed: " << st.message << "\n";
    return 1;
  }
  mvdb::Executor exec(db);

  std::cerr << "minivectordb: data_dir=" << opt.data_dir
            << " protocol=" << opt.protocol << " query_mode=" << opt.query_mode
            << " dml_ack=" << (opt.dml_ack ? "ok" : "none")
            << " statement_mode=" << (opt.line_mode ? "line" : "semicolon") << "\n";
  if (!opt.ready_marker.empty()) {
    bool adaptive = std::getenv("MVDB_ADAPTIVE_READY") != nullptr;
    bool defer = false;
    if (adaptive) {
      // A batch driver may already have written SQL before reading anything;
      // in that case an extra ready line would corrupt its first response.
      // A request/response driver waits, so the poll times out and we print.
      struct pollfd pfd {};
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      defer = (::poll(&pfd, 1, 50) > 0);
    }
    if (!defer) writeOut(opt.ready_marker + "\n");
  }

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
        mvdb::ParseResult pending = mvdb::parseStatement(buffer);
        if (pending.kind == mvdb::ParseResult::Kind::Ok && !stdinHasData(10)) {
          stop = !drainBuffer(buffer, exec, opt, true);
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
