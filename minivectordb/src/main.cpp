// MiniVectorDB session entry point.
//
// Transport: JSON Lines over stdin/stdout, as specified by
// 2026-competition-round2/STDIO_PROTOCOL.md.
//   stdout: {"type":"ready"} once per start, then exactly one response line per
//           request, each terminated by a real newline and written immediately.
//   stdin:  {"id":N,"op":"execute","sql":"...","search_mode":"exact"}
//           {"id":N,"op":"shutdown"}
//   stderr: diagnostics only.
#include "exec.h"
#include "mvd.h"
#include "storage.h"

#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

bool write_all(int fd, std::string_view s) {
  size_t done = 0;
  while (done < s.size()) {
    ssize_t n = ::write(fd, s.data() + done, s.size() - done);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;  // EPIPE: the judge closed the pipe.
    }
    if (n == 0) return false;
    done += (size_t)n;
  }
  return true;
}

void log_line(std::string_view s) {
  std::string line = "minivectordb: ";
  line += s;
  line.push_back('\n');
  write_all(STDERR_FILENO, line);
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace((unsigned char)s[a])) ++a;
  while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
  return std::string(s.substr(a, b - a));
}

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

// ---------------------------------------------------------------- JSON input
struct Request {
  bool parsed = false;
  std::string id_raw = "0";  // echoed verbatim so its JSON type is preserved
  std::string op;
  std::string sql;
  std::string search_mode = "exact";
};

struct Cursor {
  std::string_view s;
  size_t p = 0;

  void ws() {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n')) ++p;
  }
};

void append_utf8(std::string& out, unsigned cp) {
  if (cp < 0x80) {
    out.push_back((char)cp);
  } else if (cp < 0x800) {
    out.push_back((char)(0xC0 | (cp >> 6)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char)(0xE0 | (cp >> 12)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (cp >> 18)));
    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

bool hex4(Cursor& c, unsigned& out) {
  if (c.p + 4 > c.s.size()) return false;
  unsigned v = 0;
  for (int i = 0; i < 4; ++i) {
    char ch = c.s[c.p++];
    v <<= 4;
    if (ch >= '0' && ch <= '9') v |= (unsigned)(ch - '0');
    else if (ch >= 'a' && ch <= 'f') v |= (unsigned)(ch - 'a' + 10);
    else if (ch >= 'A' && ch <= 'F') v |= (unsigned)(ch - 'A' + 10);
    else return false;
  }
  out = v;
  return true;
}

bool parse_json_string(Cursor& c, std::string& out) {
  c.ws();
  if (c.p >= c.s.size() || c.s[c.p] != '"') return false;
  ++c.p;
  out.clear();
  while (c.p < c.s.size()) {
    char ch = c.s[c.p++];
    if (ch == '"') return true;
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (c.p >= c.s.size()) return false;
    char e = c.s[c.p++];
    switch (e) {
      case '"': out.push_back('"'); break;
      case '\\': out.push_back('\\'); break;
      case '/': out.push_back('/'); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      case 'u': {
        unsigned cp = 0;
        if (!hex4(c, cp)) return false;
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (c.p + 1 >= c.s.size() || c.s[c.p] != '\\' || c.s[c.p + 1] != 'u') return false;
          c.p += 2;
          unsigned lo = 0;
          if (!hex4(c, lo)) return false;
          if (lo < 0xDC00 || lo > 0xDFFF) return false;
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
        }
        append_utf8(out, cp);
        break;
      }
      default:
        return false;
    }
  }
  return false;
}

bool skip_value(Cursor& c) {
  c.ws();
  if (c.p >= c.s.size()) return false;
  char ch = c.s[c.p];
  if (ch == '"') {
    std::string tmp;
    return parse_json_string(c, tmp);
  }
  if (ch == '{' || ch == '[') {
    const char open = ch, close = (ch == '{') ? '}' : ']';
    int depth = 0;
    while (c.p < c.s.size()) {
      char x = c.s[c.p];
      if (x == '"') {
        std::string tmp;
        if (!parse_json_string(c, tmp)) return false;
        continue;
      }
      ++c.p;
      if (x == open) {
        ++depth;
      } else if (x == close) {
        if (--depth == 0) return true;
      }
    }
    return false;
  }
  size_t start = c.p;
  while (c.p < c.s.size() && !std::isspace((unsigned char)c.s[c.p]) && c.s[c.p] != ',' &&
         c.s[c.p] != '}')
    ++c.p;
  return c.p > start;
}

bool parse_request(std::string_view line, Request& r) {
  Cursor c{line};
  c.ws();
  if (c.p >= c.s.size() || c.s[c.p] != '{') return false;
  ++c.p;
  for (;;) {
    c.ws();
    if (c.p < c.s.size() && c.s[c.p] == '}') {
      ++c.p;
      r.parsed = true;
      return true;
    }
    std::string key;
    if (!parse_json_string(c, key)) return false;
    c.ws();
    if (c.p >= c.s.size() || c.s[c.p] != ':') return false;
    ++c.p;
    c.ws();
    if (key == "id") {
      size_t start = c.p;
      if (!skip_value(c)) return false;
      r.id_raw = trim(c.s.substr(start, c.p - start));
      if (r.id_raw.empty()) return false;
    } else if (key == "op") {
      if (!parse_json_string(c, r.op)) return false;
    } else if (key == "sql") {
      if (!parse_json_string(c, r.sql)) return false;
    } else if (key == "search_mode") {
      if (!parse_json_string(c, r.search_mode)) return false;
    } else if (!skip_value(c)) {
      return false;
    }
    c.ws();
    if (c.p < c.s.size() && c.s[c.p] == ',') {
      ++c.p;
      continue;
    }
    if (c.p < c.s.size() && c.s[c.p] == '}') {
      ++c.p;
      r.parsed = true;
      return true;
    }
    return false;
  }
}

// ---------------------------------------------------------------- JSON output
void append_cell(std::string& out, const mvd::Cell& c) {
  char buf[64];
  switch (c.kind) {
    case mvd::Cell::Kind::Int:
      std::snprintf(buf, sizeof buf, "%lld", (long long)c.i);
      out += buf;
      break;
    case mvd::Cell::Kind::Dbl:
      if (!std::isfinite(c.d)) {
        out += "null";
      } else {
        std::snprintf(buf, sizeof buf, "%.10g", c.d);
        out += buf;
      }
      break;
    case mvd::Cell::Kind::Str:
      mvd::json_escape(out, c.s);
      break;
    case mvd::Cell::Kind::Vec:
      out.push_back('[');
      for (size_t i = 0; i < c.v.size(); ++i) {
        if (i) out.push_back(',');
        if (!std::isfinite(c.v[i])) {
          out += "null";
          continue;
        }
        std::snprintf(buf, sizeof buf, "%.9g", (double)c.v[i]);
        out += buf;
      }
      out.push_back(']');
      break;
  }
}

std::string render_ok(const std::string& id_raw, const mvd::ResultSet& rs) {
  std::string out = "{\"id\":";
  out += id_raw;
  out += ",\"ok\":true";
  if (rs.query) {
    out += ",\"columns\":[";
    for (size_t i = 0; i < rs.columns.size(); ++i) {
      if (i) out.push_back(',');
      mvd::json_escape(out, rs.columns[i]);
    }
    out += "],\"rows\":[";
    for (size_t r = 0; r < rs.rows.size(); ++r) {
      if (r) out.push_back(',');
      out.push_back('[');
      for (size_t i = 0; i < rs.rows[r].size(); ++i) {
        if (i) out.push_back(',');
        append_cell(out, rs.rows[r][i]);
      }
      out.push_back(']');
    }
    out += ']';
  } else if (rs.has_affected) {
    char buf[32];
    std::snprintf(buf, sizeof buf, ",\"affected_rows\":%lld", (long long)rs.affected);
    out += buf;
  }
  out += "}\n";
  return out;
}

std::string render_error(const std::string& id_raw, std::string_view code, std::string_view message) {
  std::string out = "{\"id\":";
  out += id_raw;
  out += ",\"ok\":false,\"error\":{\"code\":";
  mvd::json_escape(out, code);
  out += ",\"message\":";
  mvd::json_escape(out, message);
  out += "}}\n";
  return out;
}

// Strips surrounding whitespace and a trailing statement terminator: the judge
// sends complete SQL strings that end with ';'.
std::string prepare_sql(std::string_view raw) {
  std::string sql = trim(raw);
  while (!sql.empty() && (sql.back() == ';' || std::isspace((unsigned char)sql.back()))) {
    sql.pop_back();
  }
  return sql;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  size_t cache_mb = 256;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "minivectordb: %s requires a value\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--data-dir" || a == "--data_dir") {
      data_dir = next("--data-dir");
    } else if (a.rfind("--data-dir=", 0) == 0) {
      data_dir = a.substr(11);
    } else if (a == "--cache-mb") {
      cache_mb = (size_t)std::strtoull(next("--cache-mb").c_str(), nullptr, 10);
    } else if (a.rfind("--cache-mb=", 0) == 0) {
      cache_mb = (size_t)std::strtoull(a.c_str() + 11, nullptr, 10);
    } else if (a == "--help" || a == "-h") {
      std::fprintf(stderr, "usage: minivectordb [--data-dir DIR] [--cache-mb N]\n");
      return 0;
    } else {
      // Unknown flags are ignored: the judge may append its own and refusing to
      // start would fail every case.
      std::fprintf(stderr, "minivectordb: ignoring unknown option %s\n", a.c_str());
    }
  }
  if (const char* env = std::getenv("VECTORDB_DATA_DIR")) {
    if (data_dir == "/data") data_dir = env;
  }
  if (const char* env = std::getenv("MVD_CACHE_MB")) {
    cache_mb = (size_t)std::strtoull(env, nullptr, 10);
  }
  if (cache_mb == 0) cache_mb = 256;
  if (size_t lim = cgroup_memory_mb(); lim >= 32) {
    size_t cap = lim / 3;
    if (cap < 8) cap = 8;
    if (cache_mb > cap) cache_mb = cap;
  }

  ::signal(SIGPIPE, SIG_IGN);
  struct sigaction sa;
  std::memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_signal;
  ::sigaction(SIGINT, &sa, nullptr);
  ::sigaction(SIGTERM, &sa, nullptr);

  try {
    std::unique_ptr<mvd::Store> store;
    try {
      store = std::make_unique<mvd::Store>(data_dir, cache_mb * 1024 * 1024);
      store->open();
    } catch (const std::exception& e) {
      log_line("data dir " + data_dir + " unusable: " + e.what());
      // A read-only container leaves only /data writable, so there is no
      // meaningful fallback: report and exit non-zero instead of pretending.
      return 1;
    }
    mvd::Executor exec(*store);

    // Recovery is complete: announce readiness before reading any request.
    if (!write_all(STDOUT_FILENO, "{\"type\":\"ready\"}\n")) return 0;
    log_line("ready (data dir " + data_dir + ", cache " + std::to_string(cache_mb) + "MB, " +
             std::to_string(store->tables().size()) + " tables)");

    std::string pending;
    char buf[65536];
    bool running = true;
    while (running && !g_stop) {
      ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
      if (n < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (n == 0) break;  // EOF: the judge closed stdin.
      pending.append(buf, (size_t)n);

      size_t nl;
      while (running && (nl = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, nl);
        pending.erase(0, nl + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        Request req;
        if (!parse_request(line, req)) {
          log_line("ignoring malformed request line");
          continue;
        }
        if (req.op == "shutdown") {
          store->rollback();
          std::string out = "{\"id\":" + req.id_raw + ",\"ok\":true}\n";
          write_all(STDOUT_FILENO, out);
          log_line("shutdown");
          running = false;
          break;
        }
        if (req.op != "execute") {
          write_all(STDOUT_FILENO, render_error(req.id_raw, "unsupported_op", "unknown op"));
          continue;
        }

        // Per-request search mode; each case starts in exact mode.
        exec.execute(req.search_mode == "ann" ? "SET vector_mode = ann" : "SET vector_mode = exact");

        std::string sql = prepare_sql(req.sql);
        if (sql.empty()) {
          mvd::ResultSet empty;
          write_all(STDOUT_FILENO, render_ok(req.id_raw, empty));
          continue;
        }

        mvd::ResultSet rs;
        try {
          rs = exec.execute(sql);
        } catch (const mvd::DbError& e) {
          log_line(std::string("error ") + mvd::err_name(e.code) + ": " + e.what());
          write_all(STDOUT_FILENO, render_error(req.id_raw, mvd::err_name(e.code), e.what()));
          continue;
        } catch (const std::exception& e) {
          log_line(std::string("error internal_error: ") + e.what());
          write_all(STDOUT_FILENO, render_error(req.id_raw, "internal_error", e.what()));
          continue;
        } catch (...) {
          write_all(STDOUT_FILENO, render_error(req.id_raw, "internal_error", "unknown failure"));
          continue;
        }

        if (rs.quit) {
          store->rollback();
          std::string out = "{\"id\":" + req.id_raw + ",\"ok\":true}\n";
          write_all(STDOUT_FILENO, out);
          log_line("shutdown (sql)");
          running = false;
          break;
        }
        if (!write_all(STDOUT_FILENO, render_ok(req.id_raw, rs))) running = false;
      }
    }
    store->rollback();
    return 0;
  } catch (const mvd::DbError& e) {
    std::fprintf(stderr, "minivectordb: fatal %s: %s\n", mvd::err_name(e.code), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "minivectordb: fatal: %s\n", e.what());
    return 1;
  }
}
