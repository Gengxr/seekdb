// Statement planning and execution on top of Store.
#pragma once

#include "mvd.h"
#include "storage.h"

namespace mvd {

struct Cell {
  enum class Kind { Int, Str, Dbl } kind = Kind::Int;
  int64_t i = 0;
  double d = 0;
  std::string s;

  static Cell big(int64_t v) {
    Cell c;
    c.kind = Kind::Int;
    c.i = v;
    return c;
  }
  static Cell dbl(double v) {
    Cell c;
    c.kind = Kind::Dbl;
    c.d = v;
    return c;
  }
  static Cell str(std::string v) {
    Cell c;
    c.kind = Kind::Str;
    c.s = std::move(v);
    return c;
  }
};

struct ResultSet {
  bool query = false;
  bool has_affected = false;
  bool quit = false;
  std::vector<std::string> columns;
  std::vector<std::vector<Cell>> rows;
  int64_t affected = 0;
};

class Executor {
 public:
  explicit Executor(Store& store) : store_(store) {}
  ResultSet execute(std::string_view sql);
  bool ann_mode() const { return ann_mode_; }

 private:
  Store& store_;
  bool ann_mode_ = false;

  struct ItemPlan {
    bool dist = false;
    int32_t col = -1;
    std::vector<float> query;
    std::string name;
  };

  struct CondPlan {
    int32_t col = -1;
    CmpOp op = CmpOp::Eq;
    Value lit;
  };

  struct RowRef {
    int64_t pk = 0;
    uint32_t slot = UINT32_MAX;
    uint32_t overlay = UINT32_MAX;
  };

  ResultSet exec_select(Stmt& stmt);
  ResultSet exec_insert(Stmt& stmt);
  ResultSet exec_update(Stmt& stmt);
  ResultSet exec_delete(Stmt& stmt);

  void resolve_conditions(const Schema& schema, const std::vector<Cond>& in, std::vector<CondPlan>& out);
  Value coerce_literal(const Column& col, const RawValue& raw);
  std::vector<RowRef> collect_matching(Table& t, const std::vector<CondPlan>& conds);
  bool match_row(const Schema& schema, const std::vector<CondPlan>& conds, int64_t pk, const RowView& rv);
};

}  // namespace mvd
