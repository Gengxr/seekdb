// SQL lexer + recursive-descent parser for the MiniVectorDB subset.
#include "mvd.h"

#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace mvd {

const char* err_name(ErrCode c) {
  switch (c) {
    case ErrCode::Ok: return "ok";
    case ErrCode::DuplicateKey: return "duplicate_key";
    case ErrCode::UnsupportedSql: return "unsupported_sql";
    case ErrCode::SyntaxError: return "syntax_error";
    case ErrCode::DimensionMismatch: return "dimension_mismatch";
    case ErrCode::InvalidVector: return "invalid_vector";
    case ErrCode::InvalidValue: return "invalid_value";
    case ErrCode::TableNotFound: return "table_not_found";
    case ErrCode::TableExists: return "table_exists";
    case ErrCode::ColumnNotFound: return "column_not_found";
    case ErrCode::ColumnExists: return "column_exists";
    case ErrCode::IndexExists: return "index_exists";
    case ErrCode::PrimaryKeyMissing: return "primary_key_missing";
    case ErrCode::StringTooLong: return "string_too_long";
    case ErrCode::IntegerOverflow: return "integer_overflow";
    case ErrCode::TooManyColumns: return "too_many_columns";
    case ErrCode::TransactionActive: return "transaction_active";
    case ErrCode::NoTransaction: return "no_transaction";
    case ErrCode::IoError: return "io_error";
    case ErrCode::Internal: return "internal_error";
  }
  return "internal_error";
}

std::string to_lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) c = (char)std::tolower((unsigned char)c);
  return r;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  }
  return true;
}

size_t utf8_length(std::string_view s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    size_t adv = 1;
    if (c < 0x80) adv = 1;
    else if ((c >> 5) == 0x6) adv = 2;
    else if ((c >> 4) == 0xE) adv = 3;
    else if ((c >> 3) == 0x1E) adv = 4;
    if (i + adv > s.size()) adv = 1;
    i += adv;
    ++n;
  }
  return n;
}

void json_escape(std::string& out, std::string_view s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out.push_back('"');
}

void json_double(std::string& out, double v) {
  if (!std::isfinite(v)) {
    out += "null";
    return;
  }
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.6f", v);
  out += buf;
}

int64_t now_nanos() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

uint32_t crc32_bytes(const void* data, size_t len) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    init = true;
  }
  const unsigned char* p = (const unsigned char*)data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- lexer
namespace {

struct Token {
  enum Kind { Ident, Int, Float, Str, Punct, End } kind = End;
  std::string text;
  int64_t ival = 0;
  double fval = 0;
  // Set when the literal is the magnitude 9223372036854775808, which only fits
  // in an int64 once the surrounding minus sign turns it into INT64_MIN.
  bool int_min_mag = false;
};

class Lexer {
 public:
  explicit Lexer(std::string_view src) : s_(src) {}

  std::vector<Token> run() {
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (std::isspace((unsigned char)c)) {
        ++p_;
        continue;
      }
      if (c == '-' && p_ + 1 < s_.size() && s_[p_ + 1] == '-') {
        while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        continue;
      }
      if (c == '#') {
        while (p_ < s_.size() && s_[p_] != '\n') ++p_;
        continue;
      }
      if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '*') {
        p_ += 2;
        while (p_ + 1 < s_.size() && !(s_[p_] == '*' && s_[p_ + 1] == '/')) ++p_;
        p_ = std::min(s_.size(), p_ + 2);
        continue;
      }
      if (c == '\'' || c == '"') {
        read_string(c);
        continue;
      }
      if (c == '`') {
        ++p_;
        size_t start = p_;
        while (p_ < s_.size() && s_[p_] != '`') ++p_;
        Token t;
        t.kind = Token::Ident;
        t.text = std::string(s_.substr(start, p_ - start));
        if (p_ < s_.size()) ++p_;
        toks_.push_back(std::move(t));
        continue;
      }
      if (std::isdigit((unsigned char)c) || c == '.') {
        read_number();
        continue;
      }
      if (std::isalpha((unsigned char)c) || c == '_') {
        size_t start = p_;
        while (p_ < s_.size() && (std::isalnum((unsigned char)s_[p_]) || s_[p_] == '_')) ++p_;
        Token t;
        t.kind = Token::Ident;
        t.text = std::string(s_.substr(start, p_ - start));
        toks_.push_back(std::move(t));
        continue;
      }
      Token t;
      t.kind = Token::Punct;
      t.text = std::string(1, c);
      toks_.push_back(std::move(t));
      ++p_;
    }
    Token e;
    e.kind = Token::End;
    toks_.push_back(e);
    return std::move(toks_);
  }

 private:
  std::string_view s_;
  size_t p_ = 0;
  std::vector<Token> toks_;

  void read_string(char quote) {
    ++p_;
    std::string out;
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (c == quote) {
        if (p_ + 1 < s_.size() && s_[p_ + 1] == quote) {
          out.push_back(quote);
          p_ += 2;
          continue;
        }
        ++p_;
        Token t;
        t.kind = Token::Str;
        t.text = std::move(out);
        toks_.push_back(std::move(t));
        return;
      }
      if (c == '\\' && p_ + 1 < s_.size()) {
        char n = s_[p_ + 1];
        switch (n) {
          case 'n': out.push_back('\n'); break;
          case 't': out.push_back('\t'); break;
          case 'r': out.push_back('\r'); break;
          case '0': out.push_back('\0'); break;
          case '\\': out.push_back('\\'); break;
          case '\'': out.push_back('\''); break;
          case '"': out.push_back('"'); break;
          default: out.push_back(n); break;
        }
        p_ += 2;
        continue;
      }
      out.push_back(c);
      ++p_;
    }
    fail(ErrCode::SyntaxError, "unterminated string literal");
  }

  void read_number() {
    size_t start = p_;
    bool is_float = false;
    while (p_ < s_.size() && std::isdigit((unsigned char)s_[p_])) ++p_;
    if (p_ < s_.size() && s_[p_] == '.') {
      is_float = true;
      ++p_;
      while (p_ < s_.size() && std::isdigit((unsigned char)s_[p_])) ++p_;
    }
    if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
      size_t save = p_;
      ++p_;
      if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
      if (p_ < s_.size() && std::isdigit((unsigned char)s_[p_])) {
        is_float = true;
        while (p_ < s_.size() && std::isdigit((unsigned char)s_[p_])) ++p_;
      } else {
        p_ = save;
      }
    }
    std::string text(s_.substr(start, p_ - start));
    if (text == ".") {  // lone dot is punctuation
      Token t;
      t.kind = Token::Punct;
      t.text = ".";
      toks_.push_back(std::move(t));
      return;
    }
    Token t;
    if (is_float) {
      t.kind = Token::Float;
      t.fval = std::strtod(text.c_str(), nullptr);
      t.text = std::move(text);
    } else {
      t.kind = Token::Int;
      errno = 0;
      char* endp = nullptr;
      long long v = std::strtoll(text.c_str(), &endp, 10);
      if (errno == ERANGE) {
        // INT64_MIN (-9223372036854775808) has a magnitude larger than INT64_MAX,
        // so the lexer only sees the unsigned magnitude; the sign is applied by
        // the parser, which knows whether a minus sign precedes the literal.
        if (text == "9223372036854775808") {
          t.int_min_mag = true;
          t.ival = INT64_MIN;
        } else {
          fail(ErrCode::IntegerOverflow, "integer literal out of range");
        }
      } else {
        t.ival = (int64_t)v;
      }
      t.text = std::move(text);
    }
    toks_.push_back(std::move(t));
  }
};

// ---------------------------------------------------------------- parser
class Parser {
 public:
  explicit Parser(const std::vector<Token>& toks) : t_(toks) {}

  Stmt parse() {
    Stmt s;
    if (cur().kind == Token::End) return s;
    if (cur().kind != Token::Ident) fail(ErrCode::SyntaxError, "expected statement keyword");
    std::string kw = to_lower(cur().text);
    if (kw == "create") s = parse_create();
    else if (kw == "insert") s = parse_insert();
    else if (kw == "select") s = parse_select();
    else if (kw == "update") s = parse_update();
    else if (kw == "delete") s = parse_delete();
    else if (kw == "begin" || (kw == "start" && peek_kw(1, "transaction"))) {
      ++i_;
      if (kw == "start") ++i_;
      accept_kw("work");
      s.kind = Stmt::Kind::Begin;
    } else if (kw == "commit" || (kw == "end" && peek_kw(1, "work"))) {
      ++i_;
      accept_kw("work");
      s.kind = Stmt::Kind::Commit;
    } else if (kw == "rollback") {
      ++i_;
      accept_kw("work");
      s.kind = Stmt::Kind::Rollback;
    } else if (kw == "set") {
      s = parse_set();
    } else if (kw == "quit" || kw == "exit") {
      ++i_;
      s.kind = Stmt::Kind::Quit;
    } else if (kw == "ping" || kw == "noop") {
      ++i_;
      s.kind = Stmt::Kind::Noop;
    } else {
      fail(ErrCode::UnsupportedSql, "unsupported statement: " + cur().text);
    }
    if (cur().kind != Token::End) fail(ErrCode::SyntaxError, "trailing tokens: " + cur().text);
    return s;
  }

 private:
  const std::vector<Token>& t_;
  size_t i_ = 0;

  const Token& cur() const { return t_[i_]; }
  size_t idx() const { return i_; }
  void advance() {
    if (i_ + 1 < t_.size()) ++i_;
  }
  bool is_kw(std::string_view kw) const { return cur().kind == Token::Ident && iequals(cur().text, kw); }
  bool peek_kw(size_t n, std::string_view kw) const {
    size_t j = i_ + n;
    return j < t_.size() && t_[j].kind == Token::Ident && iequals(t_[j].text, kw);
  }
  bool accept_kw(std::string_view kw) {
    if (!is_kw(kw)) return false;
    advance();
    return true;
  }
  void expect_kw(std::string_view kw) {
    if (!accept_kw(kw)) fail(ErrCode::SyntaxError, "expected keyword " + std::string(kw));
  }
  bool is_punct(char c) const { return cur().kind == Token::Punct && cur().text.size() == 1 && cur().text[0] == c; }
  bool accept_punct(char c) {
    if (!is_punct(c)) return false;
    advance();
    return true;
  }
  void expect_punct(char c) {
    if (!accept_punct(c)) fail(ErrCode::SyntaxError, std::string("expected '") + c + "'");
  }
  std::string expect_ident() {
    if (cur().kind != Token::Ident) fail(ErrCode::SyntaxError, "expected identifier");
    std::string r = cur().text;
    advance();
    return r;
  }

  static float checked_float(double val) {
    if (!std::isfinite(val)) fail(ErrCode::InvalidVector, "non-finite vector element");
    if (std::fabs(val) > (double)std::numeric_limits<float>::max()) {
      fail(ErrCode::InvalidVector, "vector element overflows float32");
    }
    float f = (float)val;
    if (!std::isfinite(f)) fail(ErrCode::InvalidVector, "vector element overflows float32");
    return f;
  }

  std::vector<float> parse_vector_brackets() {
    expect_punct('[');
    std::vector<float> out;
    if (accept_punct(']')) return out;
    for (;;) {
      double sign = 1;
      if (accept_punct('-')) sign = -1;
      else accept_punct('+');
      double val = 0;
      if (cur().kind == Token::Int) {
        val = cur().int_min_mag ? 9223372036854775808.0 : (double)cur().ival;
        advance();
      } else if (cur().kind == Token::Float) {
        val = cur().fval;
        advance();
      } else {
        fail(ErrCode::InvalidVector, "expected numeric vector element");
      }
      out.push_back(checked_float(sign * val));
      if (accept_punct(',')) continue;
      expect_punct(']');
      break;
    }
    return out;
  }

  static void skip_ws(std::string_view t, size_t& p) {
    while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
  }

  std::vector<float> parse_vector_string(std::string_view text) { return parse_vector_literal(text); }

  RawValue parse_raw_value() {
    RawValue v;
    if (cur().kind == Token::Str) {
      v.kind = RawValue::Kind::Str;
      v.s = cur().text;
      advance();
      return v;
    }
    if (is_punct('[')) {
      v.kind = RawValue::Kind::Vec;
      v.v = parse_vector_brackets();
      return v;
    }
    double sign = 1;
    if (accept_punct('-')) sign = -1;
    else accept_punct('+');
    if (cur().kind == Token::Int) {
      const bool min_mag = cur().int_min_mag;
      const int64_t x = cur().ival;
      advance();
      v.kind = RawValue::Kind::Int;
      if (min_mag) {
        // Magnitude 2^63 is only representable as INT64_MIN, i.e. with a minus.
        if (sign >= 0) fail(ErrCode::IntegerOverflow, "integer literal out of range");
        v.i = x;
      } else {
        v.i = (sign < 0) ? -x : x;
      }
      return v;
    }
    if (cur().kind == Token::Float) {
      double d = sign * cur().fval;
      advance();
      if (d != std::floor(d)) fail(ErrCode::InvalidValue, "non-integral value for BIGINT");
      if (d > 9.2e18 || d < -9.2e18) fail(ErrCode::IntegerOverflow, "integer out of range");
      v.kind = RawValue::Kind::Int;
      v.i = (int64_t)d;
      return v;
    }
    if (is_kw("null")) fail(ErrCode::UnsupportedSql, "NULL is not supported");
    fail(ErrCode::SyntaxError, "expected literal value");
  }

  ColType parse_col_type(int32_t& len) {
    if (cur().kind != Token::Ident) fail(ErrCode::SyntaxError, "expected column type");
    std::string t = to_lower(cur().text);
    advance();
    len = 0;
    if (t == "bigint" || t == "int" || t == "integer" || t == "int64" || t == "long") return ColType::BigInt;
    if (t == "varchar" || t == "char" || t == "text" || t == "string") {
      if (accept_punct('(')) {
        if (cur().kind != Token::Int) fail(ErrCode::SyntaxError, "expected VARCHAR length");
        len = (int32_t)cur().ival;
        advance();
        expect_punct(')');
      } else {
        len = 65535;
      }
      if (len <= 0) fail(ErrCode::SyntaxError, "VARCHAR length must be positive");
      return ColType::VarChar;
    }
    if (t == "vector") {
      expect_punct('(');
      if (cur().kind != Token::Int) fail(ErrCode::SyntaxError, "expected VECTOR dimension");
      len = (int32_t)cur().ival;
      advance();
      expect_punct(')');
      if (len <= 0) fail(ErrCode::SyntaxError, "VECTOR dimension must be positive");
      return ColType::Vector;
    }
    fail(ErrCode::UnsupportedSql, "unsupported column type: " + t);
  }

  Stmt parse_create() {
    expect_kw("create");
    Stmt s;
    if (accept_kw("vector")) {
      expect_kw("index");
      return parse_create_index(s);
    }
    if (accept_kw("index")) return parse_create_index(s);
    if (accept_kw("table")) {
      // fallthrough
    } else if (accept_kw("unique")) {
      expect_kw("table");
    } else {
      fail(ErrCode::UnsupportedSql, "unsupported CREATE object");
    }
    s.kind = Stmt::Kind::CreateTable;
    if (accept_kw("if")) {
      expect_kw("not");
      expect_kw("exists");
    }
    s.new_table = expect_ident();
    expect_punct('(');
    int32_t pk = -1;
    std::string pk_name;
    for (;;) {
      if (is_kw("primary")) {
        advance();
        expect_kw("key");
        expect_punct('(');
        pk_name = expect_ident();
        expect_punct(')');
      } else if (is_kw("key") || is_kw("index") || is_kw("unique") || is_kw("constraint") ||
                 is_kw("foreign") || is_kw("check")) {
        fail(ErrCode::UnsupportedSql, "unsupported table constraint");
      } else {
        Column c;
        c.name = expect_ident();
        c.type = parse_col_type(c.len);
        bool is_pk = false;
        for (;;) {
          if (accept_kw("primary")) {
            expect_kw("key");
            is_pk = true;
            continue;
          }
          if (accept_kw("not")) {
            expect_kw("null");
            continue;
          }
          if (accept_kw("null")) continue;
          break;
        }
        if (s.new_schema.find(c.name) >= 0) fail(ErrCode::ColumnExists, "duplicate column " + c.name);
        s.new_schema.cols.push_back(std::move(c));
        if (is_pk) {
          if (pk >= 0) fail(ErrCode::SyntaxError, "multiple primary keys");
          pk = (int32_t)s.new_schema.cols.size() - 1;
        }
      }
      if (accept_punct(',')) continue;
      expect_punct(')');
      break;
    }
    if (!pk_name.empty()) {
      pk = s.new_schema.find(pk_name);
      if (pk < 0) fail(ErrCode::ColumnNotFound, "primary key column not found: " + pk_name);
    }
    if (pk < 0) fail(ErrCode::PrimaryKeyMissing, "table requires a BIGINT primary key");
    if (s.new_schema.cols[pk].type != ColType::BigInt) {
      fail(ErrCode::UnsupportedSql, "primary key must be BIGINT");
    }
    s.new_schema.pk_idx = pk;
    int vec_count = 0;
    for (size_t i = 0; i < s.new_schema.cols.size(); ++i) {
      if (s.new_schema.cols[i].type == ColType::Vector) {
        ++vec_count;
        s.new_schema.vec_idx = (int32_t)i;
      }
    }
    if (vec_count > 1) fail(ErrCode::UnsupportedSql, "at most one VECTOR column per table");
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported CREATE TABLE options");
    return s;
  }

  Stmt parse_create_index(Stmt& s) {
    s.kind = Stmt::Kind::CreateIndex;
    if (accept_kw("if")) {
      expect_kw("not");
      expect_kw("exists");
    }
    s.index_name = expect_ident();
    expect_kw("on");
    s.new_table = expect_ident();
    expect_punct('(');
    Column c;
    c.name = expect_ident();
    s.new_schema.cols.clear();
    s.new_schema.cols.push_back(c);
    expect_punct(')');
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported CREATE INDEX options");
    return s;
  }

  Stmt parse_insert() {
    expect_kw("insert");
    expect_kw("into");
    Stmt s;
    s.kind = Stmt::Kind::Insert;
    s.table = expect_ident();
    if (accept_punct('(')) {
      for (;;) {
        s.insert_cols.push_back(expect_ident());
        if (accept_punct(',')) continue;
        expect_punct(')');
        break;
      }
    }
    if (!accept_kw("values")) fail(ErrCode::UnsupportedSql, "only INSERT ... VALUES is supported");
    for (;;) {
      expect_punct('(');
      std::vector<RawValue> row;
      if (!is_punct(')')) {
        for (;;) {
          row.push_back(parse_raw_value());
          if (accept_punct(',')) continue;
          break;
        }
      }
      expect_punct(')');
      s.insert_rows.push_back(std::move(row));
      if (accept_punct(',')) continue;
      break;
    }
    if (is_kw("on")) fail(ErrCode::UnsupportedSql, "INSERT ... ON DUPLICATE KEY is not supported");
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported INSERT clause");
    return s;
  }

  void parse_optional_alias(SelectItem& it) {
    if (accept_kw("as")) {
      it.alias = expect_ident();
      return;
    }
    if (cur().kind == Token::Ident && !is_reserved(cur().text)) {
      it.alias = cur().text;
      advance();
    }
  }

  static bool is_reserved(std::string_view w) {
    static const char* words[] = {"from", "where", "order", "limit", "and", "or", "asc", "desc",
                                  "group", "having", "join", "on", "as", "values", "set", "into"};
    for (const char* x : words) {
      if (iequals(w, x)) return true;
    }
    return false;
  }

  Stmt parse_select() {
    expect_kw("select");
    Stmt s;
    s.kind = Stmt::Kind::Select;
    for (;;) {
      SelectItem it;
      if (accept_punct('*')) {
        it.star = true;
      } else if (cur().kind == Token::Int || cur().kind == Token::Float || cur().kind == Token::Str ||
                 is_punct('[')) {
        // Constant projection such as SELECT 1 (used by drivers as a health probe).
        it.is_literal = true;
        it.lit = parse_raw_value();
        parse_optional_alias(it);
      } else if (cur().kind == Token::Ident) {
        std::string fn = to_lower(cur().text);
        if (fn == "l2_distance") {
          advance();
          it.is_distance = true;
          expect_punct('(');
          it.col_name = expect_ident();
          expect_punct(',');
          if (cur().kind == Token::Str) {
            it.query = parse_vector_string(cur().text);
            advance();
          } else if (is_punct('[')) {
            it.query = parse_vector_brackets();
          } else {
            fail(ErrCode::SyntaxError, "expected query vector literal");
          }
          expect_punct(')');
          parse_optional_alias(it);
        } else if (fn == "l2_distance_squared" || fn == "cosine_distance" || fn == "inner_product" ||
                   fn == "dot_product") {
          fail(ErrCode::UnsupportedSql, "unsupported distance function " + fn);
        } else {
          it.col_name = cur().text;
          advance();
          parse_optional_alias(it);
        }
      } else {
        fail(ErrCode::SyntaxError, "unsupported SELECT expression");
      }
      s.items.push_back(std::move(it));
      if (accept_punct(',')) continue;
      break;
    }
    // Constant projection: SELECT 1 [, 2 ...] with no FROM clause.
    if (!is_kw("from")) {
      for (const SelectItem& it : s.items) {
        if (!it.is_literal) fail(ErrCode::SyntaxError, "expected FROM");
      }
      if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported trailing clause");
      return s;
    }
    expect_kw("from");
    s.table = expect_ident();
    if (accept_punct('.')) s.table = expect_ident();
    if (is_punct(',')) fail(ErrCode::UnsupportedSql, "multiple tables are not supported");
    if (cur().kind == Token::Ident && !is_reserved(cur().text)) {
      fail(ErrCode::UnsupportedSql, "table alias is not supported");
    }
    if (accept_kw("where")) parse_where(s);
    if (is_kw("group") || is_kw("having") || is_kw("union") || is_kw("distinct")) {
      fail(ErrCode::UnsupportedSql, "unsupported SELECT clause");
    }
    if (accept_kw("order")) {
      expect_kw("by");
      if (cur().kind == Token::Int) {
        s.order_position = (int32_t)cur().ival;
        advance();
      } else if (cur().kind == Token::Ident && to_lower(cur().text) == "l2_distance") {
        // ORDER BY L2_DISTANCE(col, '[...]'): the key expression need not be projected.
        advance();
        s.order_is_distance = true;
        expect_punct('(');
        s.order_dist_col = expect_ident();
        expect_punct(',');
        if (cur().kind == Token::Str) {
          s.order_dist_query = parse_vector_string(cur().text);
          advance();
        } else if (is_punct('[')) {
          s.order_dist_query = parse_vector_brackets();
        } else {
          fail(ErrCode::SyntaxError, "expected query vector literal");
        }
        expect_punct(')');
      } else {
        s.order_name = expect_ident();
        if (!s.order_name.empty() && accept_punct('.')) s.order_name = expect_ident();
      }
      if (accept_kw("asc")) s.order_dir = SortDir::Asc;
      else if (accept_kw("desc")) s.order_dir = SortDir::Desc;
    }
    if (accept_kw("limit")) {
      if (cur().kind != Token::Int) fail(ErrCode::SyntaxError, "LIMIT requires an integer");
      s.has_limit = true;
      s.limit = cur().ival;
      advance();
      if (s.has_limit && s.limit < 0) fail(ErrCode::SyntaxError, "LIMIT must be non-negative");
      if (is_kw("offset")) fail(ErrCode::UnsupportedSql, "OFFSET is not supported");
      if (is_punct(',')) fail(ErrCode::UnsupportedSql, "LIMIT a,b is not supported");
    }
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported trailing SELECT clause");
    return s;
  }

  void parse_where(Stmt& s) {
    for (;;) {
      Cond c;
      c.col_name = expect_ident();
      if (accept_punct('.')) c.col_name = expect_ident();
      if (accept_punct('=')) {
        c.op = CmpOp::Eq;
      } else if (is_punct('<')) {
        advance();
        c.op = accept_punct('=') ? CmpOp::Le : CmpOp::Lt;
      } else if (is_punct('>')) {
        advance();
        c.op = accept_punct('=') ? CmpOp::Ge : CmpOp::Gt;
      } else if (is_kw("is")) {
        fail(ErrCode::UnsupportedSql, "IS NULL is not supported");
      } else {
        fail(ErrCode::SyntaxError, "expected comparison operator in WHERE");
      }
      c.lit = parse_raw_value();
      s.where.push_back(std::move(c));
      if (accept_kw("and")) continue;
      if (is_kw("or") || is_kw("not") || is_kw("in") || is_kw("like") || is_kw("between")) {
        fail(ErrCode::UnsupportedSql, "only AND is supported in WHERE");
      }
      break;
    }
  }

  Stmt parse_update() {
    expect_kw("update");
    Stmt s;
    s.kind = Stmt::Kind::Update;
    s.table = expect_ident();
    expect_kw("set");
    for (;;) {
      std::string col = expect_ident();
      expect_punct('=');
      s.sets.emplace_back(std::move(col), parse_raw_value());
      if (accept_punct(',')) continue;
      break;
    }
    if (accept_kw("where")) parse_where(s);
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported UPDATE clause");
    return s;
  }

  Stmt parse_delete() {
    expect_kw("delete");
    expect_kw("from");
    Stmt s;
    s.kind = Stmt::Kind::Delete;
    s.table = expect_ident();
    if (accept_kw("where")) parse_where(s);
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported DELETE clause");
    return s;
  }

  Stmt parse_set() {
    expect_kw("set");
    Stmt s;
    s.kind = Stmt::Kind::SetMode;
    if (cur().kind != Token::Ident) fail(ErrCode::SyntaxError, "expected SET variable");
    std::string var = to_lower(cur().text);
    advance();
    if (var != "vector_mode" && var != "mode" && var != "vector_search_mode" && var != "search_mode" &&
        var != "ann") {
      fail(ErrCode::UnsupportedSql, "unsupported SET variable " + var);
    }
    accept_punct('=');
    std::string val;
    if (cur().kind == Token::Ident || cur().kind == Token::Str) val = to_lower(cur().text);
    else fail(ErrCode::SyntaxError, "expected mode value");
    advance();
    if (val == "ann" || val == "approximate" || val == "approx") s.ann_mode = true;
    else if (val == "exact" || val == "bruteforce" || val == "brute_force") s.ann_mode = false;
    else fail(ErrCode::UnsupportedSql, "unsupported vector mode " + val);
    if (cur().kind != Token::End) fail(ErrCode::UnsupportedSql, "unsupported SET clause");
    return s;
  }
};

}  // namespace

static void skip_vector_ws(std::string_view t, size_t& p) {
  while (p < t.size() && std::isspace((unsigned char)t[p])) ++p;
}

static float vector_element(double val) {
  if (!std::isfinite(val)) fail(ErrCode::InvalidVector, "non-finite vector element");
  if (std::fabs(val) > (double)std::numeric_limits<float>::max()) {
    fail(ErrCode::InvalidVector, "vector element overflows float32");
  }
  return (float)val;
}

// Powers of ten that are exactly representable as doubles, used by the
// correctly-rounded fast path below.
constexpr double kPow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                             1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

// Convert an already validated [-+]digits[.digits][e[+-]digits] span to double
// without allocating. Clinger's fast path covers the short literals a vector
// column actually contains: a mantissa below 2^53 scaled by one exact power of
// ten needs a single IEEE operation, which rounds once and therefore matches
// strtod bit for bit. Returns false where the original code failed with ERANGE
// (overflow, and results that underflow into subnormals or zero).
static bool decimal_to_double(const char* b, const char* e, double* out) {
  const char* p = b;
  bool neg = false;
  if (p < e && (*p == '-' || *p == '+')) {
    neg = (*p == '-');
    ++p;
  }
  uint64_t mant = 0;
  int64_t exp10 = 0;
  bool dropped = false;  // significant digits that did not fit in the mantissa
  auto accumulate = [&](char c, bool frac) {
    if (mant <= (UINT64_MAX - 9) / 10) {
      mant = mant * 10 + (uint64_t)(c - '0');
      if (frac) --exp10;
    } else if (c != '0') {
      dropped = true;
      if (!frac) ++exp10;
    }
  };
  for (; p < e && *p >= '0' && *p <= '9'; ++p) accumulate(*p, false);
  if (p < e && *p == '.') {
    for (++p; p < e && *p >= '0' && *p <= '9'; ++p) accumulate(*p, true);
  }
  if (p < e && (*p == 'e' || *p == 'E')) {
    ++p;
    bool eneg = false;
    if (p < e && (*p == '-' || *p == '+')) {
      eneg = (*p == '-');
      ++p;
    }
    int64_t ev = 0;
    for (; p < e && *p >= '0' && *p <= '9'; ++p) {
      if (ev < 100000) ev = ev * 10 + (*p - '0');
    }
    exp10 += eneg ? -ev : ev;
  }
  if (!dropped && mant <= (1ull << 53) && exp10 >= -22 && exp10 <= 22) {
    double v = (double)mant;
    if (exp10 > 0) {
      v *= kPow10[exp10];
    } else if (exp10 < 0) {
      v /= kPow10[-exp10];
    }
    *out = neg ? -v : v;
    return true;
  }
  const char* start = b;
  if (start < e && *start == '+') ++start;  // from_chars rejects a leading plus
  double v = 0.0;
  auto res = std::from_chars(start, e, v);
  if (res.ec != std::errc() || res.ptr != e) return false;
  if (v != 0.0) {
    if (std::fabs(v) < std::numeric_limits<double>::min()) return false;  // subnormal
  } else if (mant != 0) {
    return false;  // underflowed to zero
  }
  *out = v;
  return true;
}

std::vector<float> parse_vector_literal(std::string_view text) {
  std::vector<float> out;
  size_t p = 0;
  skip_vector_ws(text, p);
  if (p >= text.size() || text[p] != '[') fail(ErrCode::InvalidVector, "vector literal must start with '['");
  ++p;
  skip_vector_ws(text, p);
  if (p < text.size() && text[p] == ']') {
    ++p;
  } else {
    for (;;) {
      skip_vector_ws(text, p);
      size_t start = p;
      if (p < text.size() && (text[p] == '-' || text[p] == '+')) ++p;
      bool any = false;
      while (p < text.size() && std::isdigit((unsigned char)text[p])) {
        ++p;
        any = true;
      }
      if (p < text.size() && text[p] == '.') {
        ++p;
        while (p < text.size() && std::isdigit((unsigned char)text[p])) {
          ++p;
          any = true;
        }
      }
      if (!any) fail(ErrCode::InvalidVector, "bad vector element");
      if (p < text.size() && (text[p] == 'e' || text[p] == 'E')) {
        size_t save = p;
        ++p;
        if (p < text.size() && (text[p] == '+' || text[p] == '-')) ++p;
        if (p < text.size() && std::isdigit((unsigned char)text[p])) {
          while (p < text.size() && std::isdigit((unsigned char)text[p])) ++p;
        } else {
          p = save;
        }
      }
      const char* num_b = text.data() + start;
      const char* num_e = text.data() + p;
      double val = 0.0;
      if (!decimal_to_double(num_b, num_e, &val)) {
        fail(ErrCode::InvalidVector, std::string("bad vector element: ") + std::string(num_b, num_e));
      }
      out.push_back(vector_element(val));
      skip_vector_ws(text, p);
      if (p < text.size() && text[p] == ',') {
        ++p;
        continue;
      }
      break;
    }
    skip_vector_ws(text, p);
    if (p >= text.size() || text[p] != ']') fail(ErrCode::InvalidVector, "vector literal must end with ']'");
    ++p;
  }
  skip_vector_ws(text, p);
  if (p != text.size()) fail(ErrCode::InvalidVector, "trailing characters in vector literal");
  return out;
}

Stmt parse_statement(std::string_view sql) {
  Lexer lex(sql);
  std::vector<Token> toks = lex.run();
  Parser p(toks);
  return p.parse();
}

bool statement_parses(std::string_view sql) {
  if (sql.empty()) return false;
  try {
    parse_statement(sql);
    return true;
  } catch (const DbError&) {
    return false;
  } catch (const std::exception&) {
    return false;
  }
}

bool statement_incomplete(std::string_view sql) {
  if (sql.empty()) return true;
  std::vector<Token> toks;
  try {
    Lexer lex(sql);
    toks = lex.run();
  } catch (const std::exception&) {
    return true;  // e.g. an unterminated string literal
  }
  int depth = 0;
  for (const Token& t : toks) {
    if (t.kind == Token::Punct) {
      if (t.text == "(") ++depth;
      else if (t.text == ")") --depth;
    }
  }
  if (depth > 0) return true;
  if (toks.empty()) return true;
  // The last real token (the lexer appends End).
  size_t last = 0;
  for (size_t i = 0; i < toks.size(); ++i) {
    if (toks[i].kind != Token::End) last = i;
  }
  const Token& t = toks[last];
  if (t.kind == Token::Punct) {
    static const char* cont[] = {",", "=", "<", ">", "(", "+", "-", "*", "/", ".", "["};
    for (const char* c : cont) {
      if (t.text == c) return true;
    }
    return false;
  }
  if (t.kind == Token::Ident) {
    static const char* cont_words[] = {"values", "set", "where", "and", "or", "by", "into",
                                       "from", "select", "on", "not", "primary", "key",
                                       "order", "limit", "as", "table", "index", "create",
                                       "insert", "update", "delete", "start"};
    for (const char* w : cont_words) {
      if (iequals(t.text, w)) return true;
    }
  }
  return false;
}

}  // namespace mvd
