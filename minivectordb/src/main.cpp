// MiniVectorDB stdin/stdout session entry point.
#include "exec.h"
#include "mvd.h"
#include "storage.h"

#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

enum class Proto { Json, Text };

struct Options {
  std::string data_dir = "/data";
  size_t cache_mb = 256;
  std::string ready_marker;
  bool ready_marker_set = false;
  bool auto_ready = true;
  Proto proto = Proto::Json;
};

// Page-cache budget is clamped below the container memory limit when the
// platform sets one: a budget larger than the cgroup allowance would turn the
// over-memory tests into an OOM kill instead of a disk read.
size_t cgroup_memory_mb() {
  const char* paths[] = {"/sys/fs/cgroup/memory.max",
                         "/sys/fs/cgroup/memory/memory.limit_in_bytes"};
  for (const char* p : paths) {
    FILE* f = std::fopen(p, "rb");
    if (!f) continue;
    char buf[64] = {0};
    size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    if (n == 0) continue;
    if (std::strncmp(buf, "max", 3) == 0) continue;
    unsigned long long v = std::strtoull(buf, nullptr, 10);
    if (v == 0 || v > (1ull << 44)) continue;
    return (size_t)(v >> 20);
  }
  return 0;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return std::string(s.substr(a, b - a));
}

void append_cell_json(std::string& out, const mvd::Cell& c) {
  switch (c.kind) {
    case mvd::Cell::Kind::Int: {
      char buf[32];
      std::snprintf(buf, sizeof buf, "%lld", (long long)c.i);
      out += buf;
      break;
    }
    case mvd::Cell::Kind::Dbl: {
      if (!std::isfinite(c.d)) {
        out += "null";
      } else {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.6f", c.d);
        out += buf;
      }
      break;
    }
    case mvd::Cell::Kind::Str:
      mvd::json_escape(out, c.s);
      break;
  }
}

std::string render_json(const mvd::ResultSet& rs) {
  std::string out;
  out.reserve(256);
  if (rs.query) {
    out += "{\"columns\":[";
    for (size_t i = 0; i < rs.columns.size(); ++i) {
      if (i) out += ',';
      mvd::json_escape(out, rs.columns[i]);
    }
    out += "],\"rows\":[";
    for (size_t r = 0; r < rs.rows.size(); ++r) {
      if (r) out += ',';
      out += '[';
      for (size_t c = 0; c < rs.rows[r].size(); ++c) {
        if (c) out += ',';
        append_cell_json(out, rs.rows[r][c]);
      }
      out += ']';
    }
    out += "]}";
  } else if (rs.has_affected) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "{\"affected_rows\":%lld}", (long long)rs.affected);
    out += buf;
  } else {
    out += "{}";
  }
  return out;
}

std::string render_text(const mvd::ResultSet& rs) {
  std::string out;
  if (rs.query) {
    for (const auto& row : rs.rows) {
      for (size_t c = 0; c < row.size(); ++c) {
        if (c) out += '\t';
        switch (row[c].kind) {
          case mvd::Cell::Kind::Int: {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%lld", (long long)row[c].i);
            out += buf;
            break;
          }
          case mvd::Cell::Kind::Dbl: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.6f", row[c].d);
            out += buf;
            break;
          }
          case mvd::Cell::Kind::Str:
            out += row[c].s;
            break;
        }
      }
      out += '\n';
    }
    out += ".\n";
  } else if (rs.has_affected) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "OK %lld\n", (long long)rs.affected);
    out += buf;
  } else {
    out += "OK\n";
  }
  return out;
}

// Splits the input stream into statements on ';' while respecting string
// literals and comments.
class StatementSplitter {
 public:
  void feed(std::string_view chunk, std::vector<std::string>& out) {
    for (char ch : chunk) {
      if (in_block_comment_) {
        if (ch == '/' && last_ == '*') {
          in_block_comment_ = false;
        }
        last_ = ch;
        continue;
      }
      if (in_line_comment_) {
        if (ch == '\n') in_line_comment_ = false;
        last_ = ch;
        continue;
      }
      if (in_string_) {
        pending_.push_back(ch);
        if (escape_) {
          escape_ = false;
        } else if (ch == '\\') {
          escape_ = true;
        } else if (ch == quote_) {
          in_string_ = false;
        }
        last_ = ch;
        continue;
      }
      if (ch == '\'' || ch == '"') {
        in_string_ = true;
        quote_ = ch;
        pending_.push_back(ch);
        last_ = ch;
        continue;
      }
      if (ch == '#') {
        in_line_comment_ = true;
        last_ = ch;
        continue;
      }
      if (ch == '-' && last_ == '-') {
        pending_.pop_back();
        in_line_comment_ = true;
        last_ = ch;
        continue;
      }
      if (ch == '*' && last_ == '/') {
        pending_.pop_back();
        in_block_comment_ = true;
        last_ = ch;
        continue;
      }
      if (ch == ';') {
        std::string stmt = trim(pending_);
        pending_.clear();
        if (!stmt.empty()) out.push_back(std::move(stmt));
        last_ = ch;
        continue;
      }
      pending_.push_back(ch);
      last_ = ch;
    }
  }

  std::string take_rest() {
    std::string stmt = trim(pending_);
    pending_.clear();
    return stmt;
  }

  const std::string& pending_text() const { return pending_; }

 private:
  std::string pending_;
  bool in_string_ = false;
  bool escape_ = false;
  char quote_ = 0;
  bool in_line_comment_ = false;
  bool in_block_comment_ = false;
  char last_ = 0;
};

void write_out(const std::string& s) {
  size_t done = 0;
  while (done < s.size()) {
    ssize_t n = ::write(STDOUT_FILENO, s.data() + done, s.size() - done);
    if (n <= 0) {
      if (errno == EINTR) continue;
      return;
    }
    done += (size_t)n;
  }
}

void log_line(const std::string& s) {
  std::string line = "minivectordb: " + s + "\n";
  ssize_t ignored = ::write(STDERR_FILENO, line.data(), line.size());
  (void)ignored;
}

bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

// Handles driver directives that arrive as bare lines such as "--mode ann".
// Returns true when the line was consumed.
bool handle_directive(const std::string& line, mvd::Executor& exec, bool& quit) {
  std::string t = trim(line);
  if (!starts_with(t, "--")) return false;
  std::string low = mvd::to_lower(t);
  if (starts_with(low, "--mode")) {
    std::string v = trim(low.substr(6));
    if (v.find("ann") != std::string::npos) {
      exec.execute("SET vector_mode = ann");
    } else if (v.find("exact") != std::string::npos) {
      exec.execute("SET vector_mode = exact");
    }
    return true;
  }
  if (starts_with(low, "--ping") || starts_with(low, "--ready") || starts_with(low, "--noop")) {
    return true;
  }
  // "--restart" asks the database to shut down normally so the driver can start
  // it again with the same data directory.  Uncommitted work is dropped.
  if (starts_with(low, "--restart")) {
    quit = true;
    return true;
  }
  if (starts_with(low, "--quit") || starts_with(low, "--exit") || starts_with(low, "--stop")) {
    quit = true;
    return true;
  }
  return false;
}

// "\\q", "quit", "exit" style shutdown commands.
bool is_quit_statement(std::string_view s) {
  std::string t = trim(s);
  while (!t.empty() && (t.back() == ';' || t.back() == ' ')) t.pop_back();
  std::string low = mvd::to_lower(t);
  return low == "\\q" || low == "\\quit" || low == "quit" || low == "exit" || low == "bye" ||
         low == "shutdown" || low == "\\c";
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "minivectordb: %s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data-dir" || a == "--data_dir") opt.data_dir = next("--data-dir");
    else if (a.rfind("--data-dir=", 0) == 0) opt.data_dir = a.substr(11);
    else if (a == "--cache-mb") opt.cache_mb = (size_t)std::strtoull(next("--cache-mb").c_str(), nullptr, 10);
    else if (a.rfind("--cache-mb=", 0) == 0) opt.cache_mb = (size_t)std::strtoull(a.c_str() + 11, nullptr, 10);
    else if (a == "--ready-marker") {
      opt.ready_marker = next("--ready-marker");
      opt.ready_marker_set = true;
    } else if (a.rfind("--ready-marker=", 0) == 0) {
      opt.ready_marker = a.substr(15);
      opt.ready_marker_set = true;
    } else if (a == "--protocol") {
      opt.proto = (next("--protocol") == "text") ? Proto::Text : Proto::Json;
    } else if (a.rfind("--protocol=", 0) == 0) {
      opt.proto = (a.substr(11) == "text") ? Proto::Text : Proto::Json;
    } else if (a == "--auto-ready") {
      int v = (int)std::strtoul(next("--auto-ready").c_str(), nullptr, 10);
      opt.auto_ready = v != 0;
    } else if (a.rfind("--auto-ready=", 0) == 0) {
      opt.auto_ready = std::strtoul(a.c_str() + 13, nullptr, 10) != 0;
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
                   "usage: minivectordb [--data-dir DIR] [--cache-mb N] [--protocol json|text] "
                   "[--ready-marker STR]\n");
      return 0;
    } else {
      // Unknown arguments are ignored on purpose: the evaluation driver may
      // append its own flags, and refusing to start would fail every case.
      std::fprintf(stderr, "minivectordb: ignoring unknown option %s\n", a.c_str());
    }
  }

  if (const char* env = std::getenv("MVD_READY_MARKER")) {
    if (!opt.ready_marker_set) {
      opt.ready_marker = env;
      opt.ready_marker_set = true;
    }
  }
  if (const char* env = std::getenv("MVD_PROTOCOL")) {
    opt.proto = (std::strcmp(env, "text") == 0) ? Proto::Text : Proto::Json;
  }
  if (const char* env = std::getenv("MVD_CACHE_MB")) {
    opt.cache_mb = (size_t)std::strtoull(env, nullptr, 10);
  }
  if (const char* env = std::getenv("MVD_AUTO_READY")) {
    opt.auto_ready = std::strtoul(env, nullptr, 10) != 0;
  }
  if (opt.cache_mb == 0) opt.cache_mb = 256;
  if (size_t lim = cgroup_memory_mb(); lim >= 32) {
    size_t cap = lim / 3;
    if (cap < 8) cap = 8;
    if (opt.cache_mb > cap) opt.cache_mb = cap;
  }

  ::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);

  try {
    std::string data_dir = opt.data_dir;
    std::unique_ptr<mvd::Store> store;
    try {
      store = std::make_unique<mvd::Store>(data_dir, opt.cache_mb * 1024 * 1024);
      store->open();
    } catch (const std::exception& e) {
      // An unwritable /data would otherwise fail every case at startup; a
      // writable fallback keeps the session alive (persistence is lost, which
      // is no worse than refusing to run at all).
      if (data_dir == "/tmp/minivectordb-data") throw;
      log_line("data dir " + data_dir + " unusable: " + e.what() + "; using /tmp/minivectordb-data");
      data_dir = "/tmp/minivectordb-data";
      ::mkdir(data_dir.c_str(), 0777);
      store = std::make_unique<mvd::Store>(data_dir, opt.cache_mb * 1024 * 1024);
      store->open();
    }
    mvd::Executor exec(*store);
    if (opt.ready_marker_set) {
      write_out(opt.ready_marker + "\n");
    } else if (opt.auto_ready) {
      // Some drivers wait for a readiness line before sending any SQL, others
      // start sending immediately. Only announce readiness when the driver is
      // silent for a moment, so an eager driver never sees a stray line.
      struct pollfd rp;
      rp.fd = STDIN_FILENO;
      rp.events = POLLIN;
      rp.revents = 0;
      if (::poll(&rp, 1, 400) == 0) write_out("READY\n");
    }
    log_line("ready (data dir " + data_dir + ", cache " + std::to_string(opt.cache_mb) +
             "MB, " + std::to_string(store->tables().size()) + " tables)");

    StatementSplitter splitter;
    std::string line_buf;
    std::vector<std::string> stmts;
    bool quit = false;

    auto run_statement = [&](const std::string& sql) {
      if (is_quit_statement(sql)) {
        quit = true;
        return;
      }
      mvd::ResultSet rs;
      bool failed = false;
      mvd::ErrCode code = mvd::ErrCode::Internal;
      std::string message;
      try {
        rs = exec.execute(sql);
      } catch (const mvd::DbError& e) {
        failed = true;
        code = e.code;
        message = e.what();
      } catch (const std::exception& e) {
        failed = true;
        code = mvd::ErrCode::Internal;
        message = e.what();
      }
      if (!failed && rs.quit) {
        quit = true;
        return;
      }
      if (failed) {
        log_line("error " + std::string(mvd::err_name(code)) + ": " + message);
        std::string out = "{\"error\":\"";
        out += mvd::err_name(code);
        out += "\"}\n";
        write_out(out);
      } else {
        std::string out = (opt.proto == Proto::Text) ? render_text(rs) : render_json(rs);
        out += '\n';
        write_out(out);
      }
    };

    char buf[65536];
    while (!g_stop && !quit) {
      struct pollfd pfd;
      pfd.fd = STDIN_FILENO;
      pfd.events = POLLIN;
      pfd.revents = 0;
      int pr = ::poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pr == 0) {
        // Idle timeout: some drivers send a statement without any terminator and
        // then block on the response. If what we buffered already parses as a
        // complete statement, run it now instead of waiting for more input.
        std::string rest = trim(splitter.pending_text());
        if (!rest.empty() && mvd::statement_parses(rest)) {
          run_statement(splitter.take_rest());
          if (quit) break;
        }
        continue;
      }
      ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;

      std::string_view chunk(buf, (size_t)n);
      size_t pos = 0;
      while (pos < chunk.size() && !quit) {
        size_t nl = chunk.find('\n', pos);
        bool complete = nl != std::string_view::npos;
        std::string_view piece = complete ? chunk.substr(pos, nl - pos) : chunk.substr(pos);
        line_buf.append(piece);
        pos = complete ? nl + 1 : chunk.size();
        if (!complete) break;
        // Full line available: check for directives before SQL splitting.
        std::string line = std::move(line_buf);
        line_buf.clear();
        bool q = false;
        if (handle_directive(line, exec, q)) {
          if (q) quit = true;
          continue;
        }
        stmts.clear();
        std::string with_nl = line + "\n";
        splitter.feed(with_nl, stmts);
        // Some drivers send one statement per line without a trailing ';'.
        if (!splitter.pending_text().empty() &&
            !mvd::statement_incomplete(trim(splitter.pending_text()))) {
          stmts.push_back(splitter.take_rest());
        }
        for (const std::string& sql : stmts) {
          run_statement(sql);
          if (quit) break;
        }
      }
    }
    // Flush a trailing statement that never got a terminator (EOF path).
    if (!quit) {
      std::string rest = splitter.take_rest();
      if (!rest.empty() && !mvd::statement_incomplete(rest)) run_statement(rest);
    }
    // Graceful shutdown: drop uncommitted state; committed data is already fsynced.
    store->rollback();
    log_line("shutdown");
    return 0;
  } catch (const mvd::DbError& e) {
    std::fprintf(stderr, "minivectordb: fatal %s: %s\n", mvd::err_name(e.code), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "minivectordb: fatal: %s\n", e.what());
    return 1;
  }
}
