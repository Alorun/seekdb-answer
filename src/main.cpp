#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

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
  bool dml_ack = false;
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
      writeError(pr.status);
      buffer.clear();
      return true;
    }
    // A complete parse at end-of-line is not enough: the next line may be a
    // continuation (WHERE / ORDER BY / LIMIT). Require an explicit semicolon
    // unless the input is already at EOF.
    if (!at_eof && (pr.consumed == 0 || buffer[pr.consumed - 1] != ';')) {
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
  if (!parseArgs(argc, argv, opt)) return 0;

  mvdb::Database db(opt.data_dir);
  mvdb::Status st = db.open();
  if (!st.ok()) {
    std::cerr << "startup failed: " << st.message << "\n";
    return 1;
  }
  mvdb::Executor exec(db);

  std::cerr << "minivectordb: data_dir=" << opt.data_dir
            << " protocol=" << opt.protocol << " query_mode=" << opt.query_mode
            << " dml_ack=" << (opt.dml_ack ? "ok" : "none") << "\n";
  if (!opt.ready_marker.empty()) {
    writeOut(opt.ready_marker + "\n");
  }

  std::string buffer;
  std::string line;
  bool stop = false;
  while (!stop && !g_stop && std::getline(std::cin, line)) {
    buffer += line;
    buffer.push_back('\n');
    stop = !drainBuffer(buffer, exec, opt, false);
  }
  if (!stop && !buffer.empty()) {
    stop = !drainBuffer(buffer, exec, opt, true);
  }
  exec.shutdown();
  return 0;
}
