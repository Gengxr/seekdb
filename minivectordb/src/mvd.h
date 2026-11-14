// MiniVectorDB - a small single-node vector database.
// Shared declarations for parser, storage and execution.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <limits>

namespace mvd {

// ---------------------------------------------------------------- errors
enum class ErrCode : int {
  Ok = 0,
  DuplicateKey,
  UnsupportedSql,
  SyntaxError,
  DimensionMismatch,
  InvalidVector,
  InvalidValue,
  TableNotFound,
  TableExists,
  ColumnNotFound,
  ColumnExists,
  IndexExists,
  PrimaryKeyMissing,
  StringTooLong,
  IntegerOverflow,
  TooManyColumns,
  TransactionActive,
  NoTransaction,
  IoError,
  Internal,
};

const char* err_name(ErrCode c);

struct DbError : std::runtime_error {
  ErrCode code;
  DbError(ErrCode c, std::string msg) : std::runtime_error(std::move(msg)), code(c) {}
};

[[noreturn]] inline void fail(ErrCode c, std::string msg) { throw DbError(c, std::move(msg)); }

// ---------------------------------------------------------------- utilities
uint32_t crc32_bytes(const void* data, size_t len);
std::string to_lower(std::string_view s);
bool iequals(std::string_view a, std::string_view b);
size_t utf8_length(std::string_view s);
void json_escape(std::string& out, std::string_view s);
void json_double(std::string& out, double v);
int64_t now_nanos();

// ---------------------------------------------------------------- values
enum class ColType : uint8_t { BigInt = 1, VarChar = 2, Vector = 3 };

struct Column {
  std::string name;
  ColType type = ColType::BigInt;
  int32_t len = 0;  // VARCHAR: max characters; VECTOR: dimensions
};

struct Schema {
  std::vector<Column> cols;
  int32_t pk_idx = -1;
  int32_t vec_idx = -1;

  int32_t find(std::string_view name) const {
    for (size_t i = 0; i < cols.size(); ++i) {
      if (iequals(cols[i].name, name)) return (int32_t)i;
    }
    return -1;
  }
  int32_t find_vec() const {
    for (size_t i = 0; i < cols.size(); ++i) {
      if (cols[i].type == ColType::Vector) return (int32_t)i;
    }
    return -1;
  }
};

struct Value {
  ColType type = ColType::BigInt;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;

  static Value big(int64_t x) {
    Value r;
    r.type = ColType::BigInt;
    r.i = x;
    return r;
  }
  static Value str(std::string x) {
    Value r;
    r.type = ColType::VarChar;
    r.s = std::move(x);
    return r;
  }
  static Value vec(std::vector<float> x) {
    Value r;
    r.type = ColType::Vector;
    r.v = std::move(x);
    return r;
  }
};

using Row = std::vector<Value>;

// A literal as written in SQL, before it is checked against a column type.
struct RawValue {
  enum class Kind { Int, Str, Vec } kind = Kind::Int;
  int64_t i = 0;
  std::string s;
  std::vector<float> v;
};

// ---------------------------------------------------------------- AST
enum class CmpOp : uint8_t { Eq, Lt, Le, Gt, Ge };

struct Cond {
  std::string col_name;
  CmpOp op = CmpOp::Eq;
  RawValue lit;
};

enum class SortDir : uint8_t { Asc, Desc };

struct SelectItem {
  bool star = false;
  bool is_distance = false;
  bool is_literal = false;   // constant projection, e.g. SELECT 1
  bool is_count = false;     // COUNT(*), only valid with a FROM clause
  std::string col_name;      // plain column or the vector column of L2_DISTANCE
  std::vector<float> query;  // L2_DISTANCE query vector
  RawValue lit;              // literal for SELECT <literal>
  std::string alias;
};

struct Stmt {
  enum class Kind {
    Select,
    Insert,
    Update,
    Delete,
    CreateTable,
    CreateIndex,
    Begin,
    Commit,
    Rollback,
    SetMode,
    Quit,
    Noop,
  } kind = Kind::Noop;

  // SELECT
  std::string table;
  std::vector<SelectItem> items;
  std::vector<Cond> where;
  std::string order_name;  // empty: no ORDER BY
  int32_t order_position = -1;  // 1-based ORDER BY ordinal, -1 when absent
  bool order_is_distance = false;  // ORDER BY L2_DISTANCE(col, '[...]')
  std::string order_dist_col;
  std::vector<float> order_dist_query;
  SortDir order_dir = SortDir::Asc;
  bool has_limit = false;
  int64_t limit = 0;

  // INSERT
  std::vector<std::string> insert_cols;  // empty: all columns in schema order
  std::vector<std::vector<RawValue>> insert_rows;

  // UPDATE
  std::vector<std::pair<std::string, RawValue>> sets;

  // CREATE TABLE / CREATE INDEX
  Schema new_schema;
  std::string new_table;
  std::string index_name;

  // SET vector_mode
  bool ann_mode = false;
};

// Parse one statement (text without trailing semicolon). Throws DbError.
Stmt parse_statement(std::string_view sql);

// Parse a vector literal such as "[1, 0.5, 3]". Throws DbError on bad input.
std::vector<float> parse_vector_literal(std::string_view text);

// True when the text is a syntactically complete statement (used to accept
// line-oriented input where the driver omits the trailing semicolon).
bool statement_parses(std::string_view sql);

// True when the text is clearly still incomplete (open parenthesis, trailing
// comma/operator, trailing structural keyword). Used to decide whether a line
// without ';' should be executed or buffered.
bool statement_incomplete(std::string_view sql);

}  // namespace mvd
