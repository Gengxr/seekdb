// Statement execution: scans, vector top-K, DML and transactions.
#include "exec.h"

#include <immintrin.h>
#include <unordered_set>

namespace mvd {

namespace {

// ---------------------------------------------------------------- distance
float l2_sq_scalar(const float* a, const float* b, int n) {
  float total = 0.0f;
  for (int i = 0; i < n; ++i) {
    float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}

#if defined(__x86_64__) || defined(__i386__)
__attribute__((target("avx2,fma"))) float l2_sq_avx2(const float* a, const float* b, int n) {
  __m256 acc = _mm256_setzero_ps();
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    acc = _mm256_fmadd_ps(d, d, acc);
  }
  __m128 lo = _mm256_castps256_ps128(acc);
  __m128 hi = _mm256_extractf128_ps(acc, 1);
  __m128 s = _mm_add_ps(lo, hi);
  s = _mm_hadd_ps(s, s);
  s = _mm_hadd_ps(s, s);
  float total = _mm_cvtss_f32(s);
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}
#endif

double l2_sq_double(const float* a, const float* b, int n) {
  double total = 0.0;
  for (int i = 0; i < n; ++i) {
    double d = (double)a[i] - (double)b[i];
    total += d * d;
  }
  return total;
}

#if defined(__x86_64__) || defined(__i386__)
// Double-precision accumulation over float32 inputs: the result matches the
// reference scalar computation closely enough for the 1e-5 tolerance.
__attribute__((target("avx2,fma"))) double l2_sq_double_avx2(const float* a, const float* b, int n) {
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  int i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256d alo = _mm256_cvtps_pd(_mm256_castps256_ps128(va));
    __m256d ahi = _mm256_cvtps_pd(_mm256_extractf128_ps(va, 1));
    __m256d blo = _mm256_cvtps_pd(_mm256_castps256_ps128(vb));
    __m256d bhi = _mm256_cvtps_pd(_mm256_extractf128_ps(vb, 1));
    __m256d dlo = _mm256_sub_pd(alo, blo);
    __m256d dhi = _mm256_sub_pd(ahi, bhi);
    acc0 = _mm256_fmadd_pd(dlo, dlo, acc0);
    acc1 = _mm256_fmadd_pd(dhi, dhi, acc1);
  }
  __m256d acc = _mm256_add_pd(acc0, acc1);
  __m128d lo = _mm256_castpd256_pd128(acc);
  __m128d hi = _mm256_extractf128_pd(acc, 1);
  __m128d s = _mm_add_pd(lo, hi);
  s = _mm_hadd_pd(s, s);
  double total = _mm_cvtsd_f64(s);
  for (; i < n; ++i) {
    double d = (double)a[i] - (double)b[i];
    total += d * d;
  }
  return total;
}
#endif

using DistFn = float (*)(const float*, const float*, int);
DistFn g_dist = nullptr;

DistFn pick_distance() {
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return l2_sq_avx2;
#endif
  return l2_sq_scalar;
}

inline DistFn dist_fn() {
  if (!g_dist) g_dist = pick_distance();
  return g_dist;
}

using DistFnD = double (*)(const float*, const float*, int);
DistFnD g_distd = nullptr;

DistFnD pick_distance_double() {
#if defined(__x86_64__) || defined(__i386__)
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) return l2_sq_double_avx2;
#endif
  return l2_sq_double;
}

inline DistFnD dist_fn_double() {
  if (!g_distd) g_distd = pick_distance_double();
  return g_distd;
}

// ---------------------------------------------------------------- sort keys
struct Key {
  uint8_t kind = 0;  // 0 = int, 1 = double, 2 = string
  int64_t i = 0;
  double d = 0;
  std::string s;
};

inline int cmp_key(const Key& a, const Key& b) {
  if (a.kind != b.kind) return a.kind < b.kind ? -1 : 1;
  switch (a.kind) {
    case 0:
      return a.i < b.i ? -1 : (a.i > b.i ? 1 : 0);
    case 1:
      return a.d < b.d ? -1 : (a.d > b.d ? 1 : 0);
    default: {
      int c = a.s.compare(b.s);
      return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
  }
}

struct Cand {
  Key key;
  int64_t pk = 0;
  uint32_t slot = UINT32_MAX;
  uint32_t overlay = UINT32_MAX;
};

inline bool cand_less(const Cand& a, const Cand& b) {
  int c = cmp_key(a.key, b.key);
  if (c != 0) return c < 0;
  return a.pk < b.pk;
}

struct CandHeapCmp {
  bool operator()(const Cand& a, const Cand& b) const { return cand_less(a, b); }
};

}  // namespace

// ---------------------------------------------------------------- helpers
Value Executor::coerce_literal(const Column& col, const RawValue& raw) {
  switch (col.type) {
    case ColType::BigInt: {
      if (raw.kind != RawValue::Kind::Int) fail(ErrCode::InvalidValue, "expected integer value");
      return Value::big(raw.i);
    }
    case ColType::VarChar: {
      if (raw.kind == RawValue::Kind::Str) {
        if (utf8_length(raw.s) > (size_t)col.len) fail(ErrCode::StringTooLong, "string exceeds VARCHAR length");
        return Value::str(raw.s);
      }
      if (raw.kind == RawValue::Kind::Int) {
        std::string s = std::to_string(raw.i);
        if (utf8_length(s) > (size_t)col.len) fail(ErrCode::StringTooLong, "string exceeds VARCHAR length");
        return Value::str(std::move(s));
      }
      fail(ErrCode::InvalidValue, "vector literal for VARCHAR column");
    }
    case ColType::Vector: {
      std::vector<float> v;
      if (raw.kind == RawValue::Kind::Vec) {
        v = raw.v;
      } else if (raw.kind == RawValue::Kind::Str) {
        v = parse_vector_literal(raw.s);
      } else {
        fail(ErrCode::InvalidVector, "expected vector literal");
      }
      if ((int32_t)v.size() != col.len) fail(ErrCode::DimensionMismatch, "vector dimension mismatch");
      return Value::vec(std::move(v));
    }
  }
  fail(ErrCode::Internal, "unreachable");
}

void Executor::resolve_conditions(const Schema& schema, const std::vector<Cond>& in,
                                  std::vector<CondPlan>& out) {
  out.clear();
  for (const Cond& c : in) {
    int32_t ci = schema.find(c.col_name);
    if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + c.col_name);
    if (schema.cols[ci].type == ColType::Vector) {
      fail(ErrCode::UnsupportedSql, "filtering on a VECTOR column is not supported");
    }
    CondPlan p;
    p.col = ci;
    p.op = c.op;
    p.lit = coerce_literal(schema.cols[ci], c.lit);
    out.push_back(std::move(p));
  }
}

bool Executor::match_row(const Schema& schema, const std::vector<CondPlan>& conds, int64_t pk,
                         const RowView& rv) {
  (void)pk;
  for (const CondPlan& c : conds) {
    uint32_t n = 0;
    const char* p = column_ptr(schema, c.col, rv.data, rv.len, &n);
    if (!p) fail(ErrCode::Internal, "corrupt row");
    int cmp = 0;
    if (schema.cols[c.col].type == ColType::BigInt) {
      int64_t v = 0;
      std::memcpy(&v, p, 8);
      int64_t w = c.lit.i;
      cmp = v < w ? -1 : (v > w ? 1 : 0);
    } else {
      std::string_view v(p, n);
      std::string_view w(c.lit.s);
      int cc = v.compare(w);
      cmp = cc < 0 ? -1 : (cc > 0 ? 1 : 0);
    }
    bool ok = false;
    switch (c.op) {
      case CmpOp::Eq: ok = cmp == 0; break;
      case CmpOp::Lt: ok = cmp < 0; break;
      case CmpOp::Le: ok = cmp <= 0; break;
      case CmpOp::Gt: ok = cmp > 0; break;
      case CmpOp::Ge: ok = cmp >= 0; break;
    }
    if (!ok) return false;
  }
  return true;
}

std::vector<Executor::RowRef> Executor::collect_matching(Table& t, const std::vector<CondPlan>& conds) {
  std::vector<RowRef> out;
  const Schema& schema = t.schema;
  const OverlayTable* ov = store_.overlay_for(t);
  out.reserve((size_t)std::min<uint64_t>(t.live_rows, 1u << 20));
  RowView rv;
  for (uint32_t i = 0; i < t.slots.size(); ++i) {
    const Slot& s = t.slots[i];
    if (s.dead()) continue;
    if (ov && !ov->index.empty()) {
      auto it = ov->index.find(s.pk);
      if (it != ov->index.end()) continue;  // shadowed by an overlay entry
    }
    if (conds.empty()) {
      out.push_back(RowRef{s.pk, i, UINT32_MAX});
      continue;
    }
    store_.read_slot_row(t, s, rv);
    if (match_row(schema, conds, s.pk, rv)) out.push_back(RowRef{s.pk, i, UINT32_MAX});
  }
  if (ov) {
    for (uint32_t j = 0; j < ov->rows.size(); ++j) {
      const OverlayRow& r = ov->rows[j];
      if (r.deleted) continue;
      if (!conds.empty()) {
        RowView orv;
        orv.pk = r.pk;
        orv.data = r.enc.data();
        orv.len = (uint32_t)r.enc.size();
        orv.vec_rel = r.vec_rel;
        if (!match_row(schema, conds, r.pk, orv)) continue;
      }
      out.push_back(RowRef{r.pk, UINT32_MAX, j});
    }
  }
  return out;
}

// ---------------------------------------------------------------- top-K scan
//
// The scan keeps the k best rows in a small sorted array. Two kernels exist:
// a portable one that always pays for the exact double distance, and an AVX2
// one that first runs a cheap float32 screen and only refines the rows that can
// still make the answer. The screen is exact in the sense that matters: the
// float32 bound is inflated by kScreenMargin, which comfortably covers the
// worst-case rounding error of a float32 difference-of-squares accumulation, so
// a row is never dropped unless its exact distance is strictly worse than the
// current k-th best. The reported distance is still the exact double one.
namespace {

// (dim + 3) * 2^-24 is the relative error bound of the float32 screen for
// dim <= 4096, i.e. at most 2.4e-4; 3e-5 already covers dim <= 500.
constexpr double kScreenMargin = 1.00003;

struct TopKAcc {
  std::vector<TopKHit>* out = nullptr;
  size_t k = 0;

  bool full() const { return out->size() >= k; }
  double worst() const { return out->back().d2; }
  float cutoff() const { return (float)(worst() * kScreenMargin); }

  void offer(double d2, int64_t pk, uint32_t slot) {
    std::vector<TopKHit>& v = *out;
    if (v.size() < k) {
      v.push_back(TopKHit{d2, pk, slot});
      size_t j = v.size() - 1;
      while (j > 0 && less(v[j], v[j - 1])) {
        std::swap(v[j], v[j - 1]);
        --j;
      }
      return;
    }
    if (!(d2 < v.back().d2 || (d2 == v.back().d2 && pk < v.back().pk))) return;
    v.back() = TopKHit{d2, pk, slot};
    size_t j = v.size() - 1;
    while (j > 0 && less(v[j], v[j - 1])) {
      std::swap(v[j], v[j - 1]);
      --j;
    }
  }

 private:
  static bool less(const TopKHit& a, const TopKHit& b) {
    return a.d2 < b.d2 || (a.d2 == b.d2 && a.pk < b.pk);
  }
};

}  // namespace

void Executor::scan_range_generic(const ScanSpec& spec, uint32_t begin, uint32_t end,
                                  std::vector<TopKHit>& out) {
  TopKAcc acc{&out, spec.k};
  const std::vector<Slot>& slots = *spec.slots;
  const char* arena = spec.arena;
  const bool filtered = !spec.conds->empty();
  const int dim = spec.dim;
  DistFnD fn = dist_fn_double();
  RowView rv;
  auto visit = [&](uint32_t i) {
    const Slot& s = slots[i];
    if (s.dead()) return;
    const char* row = arena + s.arena_off;
    if (filtered) {
      rv.pk = s.pk;
      rv.data = row;
      rv.len = s.len();
      rv.vec_rel = s.vec_rel;
      if (!match_row(*spec.schema, *spec.conds, s.pk, rv)) return;
    }
    acc.offer(fn((const float*)(row + s.vec_rel), spec.query, dim), s.pk, i);
  };
  if (spec.cand) {
    for (uint32_t j = begin; j < end; ++j) visit((*spec.cand)[j]);
  } else {
    for (uint32_t i = begin; i < end; ++i) visit(i);
  }
}

#if defined(__x86_64__) || defined(__i386__)
#pragma GCC push_options
#pragma GCC target("avx2,fma")

namespace {

// Cheap float32 lower-bound style screen: differences are rounded to float
// before squaring, so the result may be slightly below the exact value, which
// the cutoff margin absorbs. Four independent accumulators keep the FMA
// dependency chain short enough that the row load bandwidth is the limit.
inline float screen_avx2(const float* a, const float* b, int n) {
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  __m256 acc2 = _mm256_setzero_ps();
  __m256 acc3 = _mm256_setzero_ps();
  int i = 0;
  for (; i + 32 <= n; i += 32) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 16), _mm256_loadu_ps(b + i + 16));
    __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(a + i + 24), _mm256_loadu_ps(b + i + 24));
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
    acc2 = _mm256_fmadd_ps(d2, d2, acc2);
    acc3 = _mm256_fmadd_ps(d3, d3, acc3);
  }
  for (; i + 8 <= n; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }
  __m256 acc = _mm256_add_ps(_mm256_add_ps(acc0, acc1), _mm256_add_ps(acc2, acc3));
  __m128 s = _mm_add_ps(_mm256_castps256_ps128(acc), _mm256_extractf128_ps(acc, 1));
  s = _mm_hadd_ps(s, s);
  s = _mm_hadd_ps(s, s);
  float total = _mm_cvtss_f32(s);
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    total += d * d;
  }
  return total;
}

// Exact squared distance, same kernel the ranking already used.
inline double exact_avx2(const float* a, const float* b, int n) { return l2_sq_double_avx2(a, b, n); }

}  // namespace

void Executor::scan_range_avx2(const ScanSpec& spec, uint32_t begin, uint32_t end,
                               std::vector<TopKHit>& out) {
  TopKAcc acc{&out, spec.k};
  const std::vector<Slot>& slots = *spec.slots;
  const char* arena = spec.arena;
  const bool filtered = !spec.conds->empty();
  const int dim = spec.dim;
  const float* q = spec.query;
  RowView rv;
  auto visit = [&](uint32_t i) {
    const Slot& s = slots[i];
    if (s.dead()) return;
    const char* row = arena + s.arena_off;
    if (filtered) {
      rv.pk = s.pk;
      rv.data = row;
      rv.len = s.len();
      rv.vec_rel = s.vec_rel;
      if (!match_row(*spec.schema, *spec.conds, s.pk, rv)) return;
    }
    const float* v = (const float*)(row + s.vec_rel);
    if (acc.full() && screen_avx2(v, q, dim) > acc.cutoff()) return;
    acc.offer(exact_avx2(v, q, dim), s.pk, i);
  };
  if (spec.cand) {
    for (uint32_t j = begin; j < end; ++j) visit((*spec.cand)[j]);
  } else {
    for (uint32_t i = begin; i < end; ++i) visit(i);
  }
}

#pragma GCC pop_options
#else
void Executor::scan_range_avx2(const ScanSpec& spec, uint32_t begin, uint32_t end,
                               std::vector<TopKHit>& out) {
  scan_range_generic(spec, begin, end, out);
}
#endif

void Executor::scan_range(const ScanSpec& spec, uint32_t begin, uint32_t end,
                          std::vector<TopKHit>& out) {
#if defined(__x86_64__) || defined(__i386__)
  static const bool kAvx2 = __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (kAvx2) {
    scan_range_avx2(spec, begin, end, out);
    return;
  }
#endif
  scan_range_generic(spec, begin, end, out);
}

// Below this many rows the wake-up and merge cost outweighs the second core.
constexpr uint32_t kParallelMinRows = 4096;

void Executor::scan_topk(const ScanSpec& spec, std::vector<TopKHit>& out) {
  const uint32_t n = (uint32_t)(spec.cand ? spec.cand->size() : spec.slots->size());
  if (scan_parallel(spec, n, out)) return;
  scan_range(spec, 0, n, out);
}

bool Executor::scan_parallel(const ScanSpec& spec, uint32_t n, std::vector<TopKHit>& out) {
  if (n < kParallelMinRows) return false;
  (void)dist_fn_double();  // resolve the lazy kernel pointer before sharing it
  uint64_t gen = 0;
  {
    std::lock_guard<std::mutex> lk(par_mu_);
    if (par_state_ == 2) return false;
    if (par_state_ == 0) {
      try {
        worker_ = std::thread(&Executor::worker_main, this);
        par_state_ = 1;
      } catch (...) {
        par_state_ = 2;
        return false;
      }
    }
    par_spec_ = &spec;
    par_begin_ = 0;
    par_end_ = n / 2;
    gen = ++par_gen_;
  }
  par_cv_.notify_one();

  std::vector<TopKHit> mine;
  std::vector<TopKHit> theirs;
  mine.reserve(spec.k);
  theirs.reserve(spec.k);
  scan_range(spec, n / 2, n, mine);

  bool ok = false;
  {
    std::unique_lock<std::mutex> lk(par_mu_);
    par_done_cv_.wait(lk, [&] { return par_done_ == gen || par_state_ == 2; });
    ok = par_state_ == 1 && par_done_ == gen;
    if (ok) theirs.swap(par_out_);
  }
  // A failed worker must never cost rows: redo its half on this thread.
  if (!ok) scan_range(spec, 0, n / 2, theirs);

  out.insert(out.end(), mine.begin(), mine.end());
  out.insert(out.end(), theirs.begin(), theirs.end());
  std::sort(out.begin(), out.end(), [](const TopKHit& a, const TopKHit& b) {
    return a.d2 < b.d2 || (a.d2 == b.d2 && a.pk < b.pk);
  });
  if (out.size() > spec.k) out.resize(spec.k);
  return true;
}

void Executor::worker_main() {
  for (;;) {
    const ScanSpec* spec = nullptr;
    uint32_t begin = 0;
    uint32_t end = 0;
    uint64_t gen = 0;
    {
      std::unique_lock<std::mutex> lk(par_mu_);
      par_cv_.wait(lk, [&] { return par_stop_ || par_gen_ != par_seen_; });
      if (par_stop_) return;
      par_seen_ = gen = par_gen_;
      spec = par_spec_;
      begin = par_begin_;
      end = par_end_;
    }
    std::vector<TopKHit> local;
    local.reserve(spec->k);
    try {
      scan_range(*spec, begin, end, local);
    } catch (...) {
      std::lock_guard<std::mutex> lk(par_mu_);
      par_state_ = 2;
      par_done_cv_.notify_all();
      return;
    }
    {
      std::lock_guard<std::mutex> lk(par_mu_);
      par_out_.swap(local);
      par_done_ = gen;
    }
    par_done_cv_.notify_all();
  }
}

void Executor::stop_worker() {
  {
    std::lock_guard<std::mutex> lk(par_mu_);
    par_stop_ = true;
  }
  par_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
}

Executor::~Executor() { stop_worker(); }

// ---------------------------------------------------------------- SELECT
ResultSet Executor::exec_select(Stmt& stmt) {
  // Constant projection (SELECT 1) used by drivers as a health probe.
  if (stmt.table.empty()) {
    ResultSet rs;
    rs.query = true;
    std::vector<Cell> row;
    for (const SelectItem& it : stmt.items) {
      if (!it.is_literal) fail(ErrCode::SyntaxError, "expected FROM");
      std::string name = it.alias;
      if (name.empty()) {
        switch (it.lit.kind) {
          case RawValue::Kind::Int:
            name = std::to_string(it.lit.i);
            break;
          case RawValue::Kind::Str:
            name = it.lit.s;
            break;
          default:
            name = "value";
            break;
        }
      }
      rs.columns.push_back(name);
      switch (it.lit.kind) {
        case RawValue::Kind::Int:
          row.push_back(Cell::big(it.lit.i));
          break;
        case RawValue::Kind::Str:
          row.push_back(Cell::str(it.lit.s));
          break;
        default:
          row.push_back(Cell::vec(it.lit.v));
          break;
      }
    }
    if (!row.empty()) rs.rows.push_back(std::move(row));
    return rs;
  }

  Table& t = store_.require_table(stmt.table);
  const Schema& schema = t.schema;

  std::vector<ItemPlan> items;
  items.reserve(stmt.items.size());
  for (const SelectItem& it : stmt.items) {
    if (it.star) {
      for (size_t c = 0; c < schema.cols.size(); ++c) {
        ItemPlan p;
        p.col = (int32_t)c;
        p.name = schema.cols[c].name;
        items.push_back(std::move(p));
      }
      continue;
    }
    ItemPlan p;
    int32_t ci = schema.find(it.col_name);
    if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + it.col_name);
    p.col = ci;
    if (it.is_distance) {
      if (schema.cols[ci].type != ColType::Vector) {
        fail(ErrCode::InvalidValue, "L2_DISTANCE requires a VECTOR column");
      }
      if ((int32_t)it.query.size() != schema.cols[ci].len) {
        fail(ErrCode::DimensionMismatch, "query vector dimension mismatch");
      }
      p.dist = true;
      p.query = it.query;
      p.name = it.alias.empty() ? "distance" : it.alias;
    } else {
      p.name = it.alias.empty() ? schema.cols[ci].name : it.alias;
    }
    items.push_back(std::move(p));
  }
  if (items.empty()) fail(ErrCode::SyntaxError, "empty projection");

  std::vector<CondPlan> conds;
  resolve_conditions(schema, stmt.where, conds);

  int order_item = -1;
  int32_t order_col = -1;
  if (stmt.order_is_distance) {
    // ORDER BY L2_DISTANCE(...) need not be projected: add it as a hidden key item.
    int32_t ci = schema.find(stmt.order_dist_col);
    if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + stmt.order_dist_col);
    if (schema.cols[ci].type != ColType::Vector) {
      fail(ErrCode::InvalidValue, "L2_DISTANCE requires a VECTOR column");
    }
    if ((int32_t)stmt.order_dist_query.size() != schema.cols[ci].len) {
      fail(ErrCode::DimensionMismatch, "query vector dimension mismatch");
    }
    int found = -1;
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].dist && !items[i].hidden && items[i].col == ci &&
          items[i].query == stmt.order_dist_query) {
        found = (int)i;
        break;
      }
    }
    if (found < 0) {
      ItemPlan p;
      p.dist = true;
      p.hidden = true;
      p.col = ci;
      p.query = stmt.order_dist_query;
      p.name = "distance";
      items.push_back(std::move(p));
      found = (int)items.size() - 1;
    }
    order_item = found;
  } else if (stmt.order_position > 0) {
    if ((size_t)stmt.order_position > items.size()) {
      fail(ErrCode::ColumnNotFound, "ORDER BY ordinal out of range");
    }
    order_item = stmt.order_position - 1;
  } else if (!stmt.order_name.empty()) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (iequals(items[i].name, stmt.order_name)) {
        order_item = (int)i;
        break;
      }
    }
    if (order_item < 0) {
      order_col = schema.find(stmt.order_name);
      if (order_col < 0) fail(ErrCode::ColumnNotFound, "ORDER BY column not found: " + stmt.order_name);
      if (schema.cols[order_col].type == ColType::Vector) {
        fail(ErrCode::UnsupportedSql, "ORDER BY a VECTOR column is not supported");
      }
    }
  }

  const int64_t limit = stmt.has_limit ? stmt.limit : -1;
  const bool asc = stmt.order_dir == SortDir::Asc;

  const bool topk = order_item >= 0 && items[order_item].dist;
  const bool use_heap = topk && limit >= 0 && asc;

  ResultSet rs;
  rs.query = true;
  for (const ItemPlan& it : items) {
    if (!it.hidden) rs.columns.push_back(it.name);
  }

  // ------------------------- fast path: vector TopK in one pass
  // limit == 0 stays on the general path: it answers with an empty result set,
  // while a zero-length accumulator has no last entry to compare against.
  if (use_heap && limit > 0 && t.cached && store_.overlay_for(t)->rows.empty()) {
    const std::vector<float>& q = items[order_item].query;
    ScanSpec spec;
    spec.arena = t.arena.data();
    spec.slots = &t.slots;
    spec.schema = &schema;
    spec.conds = &conds;
    spec.query = q.data();
    spec.dim = (int32_t)q.size();
    spec.k = (size_t)limit;
    // A single equality predicate can be answered from the column's equality
    // index, which visits only the matching slots.  Candidates are re-checked
    // against the row bytes, so a stale index entry can only cost time.
    if (conds.size() == 1 && conds[0].op == CmpOp::Eq) {
      spec.cand = store_.eq_candidates(t, conds[0].col, conds[0].lit);
    }
    std::vector<TopKHit> best;
    best.reserve(spec.k);
    scan_topk(spec, best);
    const char* arena = spec.arena;
    rs.rows.reserve(best.size());
    for (const TopKHit& b : best) {
      const Slot& s = t.slots[b.slot];
      const char* row = arena + s.arena_off;
      uint32_t len = s.len();
      std::vector<Cell> out;
      out.reserve(items.size());
      for (const ItemPlan& it : items) {
        if (it.hidden) continue;
        if (it.dist) {
          out.push_back(Cell::dbl(std::sqrt(l2_sq_double((const float*)(row + s.vec_rel), it.query.data(),
                                                          (int)it.query.size()))));
          continue;
        }
        Value v = decode_column(schema, it.col, row, len);
        switch (v.type) {
          case ColType::BigInt:
            out.push_back(Cell::big(v.i));
            break;
          case ColType::VarChar:
            out.push_back(Cell::str(std::move(v.s)));
            break;
          case ColType::Vector:
            out.push_back(Cell::vec(std::move(v.v)));
            break;
        }
      }
      rs.rows.push_back(std::move(out));
    }
    if (limit >= 0 && (int64_t)rs.rows.size() > limit) rs.rows.resize((size_t)limit);
    return rs;
  }

  std::vector<RowRef> refs = collect_matching(t, conds);

  std::vector<Cand> heap;
  std::vector<Cand> all;
  if (use_heap) heap.reserve((size_t)limit + 1);

  auto push_candidate = [&](Cand&& c) {
    if (use_heap && limit > 0) {
      if ((int64_t)heap.size() < limit) {
        heap.push_back(std::move(c));
        std::push_heap(heap.begin(), heap.end(), CandHeapCmp());
      } else if (cand_less(c, heap.front())) {
        std::pop_heap(heap.begin(), heap.end(), CandHeapCmp());
        heap.back() = std::move(c);
        std::push_heap(heap.begin(), heap.end(), CandHeapCmp());
      }
    } else if (!use_heap) {
      all.push_back(std::move(c));
    }
  };

  RowView rv;
  if (topk && !refs.empty()) {
    const int32_t vec_col = items[order_item].col;
    const std::vector<float>& q = items[order_item].query;
    const int dim = (int)q.size();
    (void)vec_col;
    DistFn fn = dist_fn();
    for (const RowRef& r : refs) {
      const char* data;
      uint32_t vec_rel;
      int64_t pk = r.pk;
      if (r.slot != UINT32_MAX) {
        store_.read_slot_row(t, t.slots[r.slot], rv);
        data = rv.data;
        vec_rel = rv.vec_rel;
      } else {
        const OverlayRow& o = store_.overlay_for(t)->rows[r.overlay];
        data = o.enc.data();
        vec_rel = o.vec_rel;
      }
      float sq = fn((const float*)(data + vec_rel), q.data(), dim);
      Cand c;
      c.pk = pk;
      c.slot = r.slot;
      c.overlay = r.overlay;
      c.key.kind = 1;
      c.key.d = (double)sq;
      push_candidate(std::move(c));
    }
  } else if (!refs.empty()) {
    for (const RowRef& r : refs) {
      const char* data;
      uint32_t len;
      uint32_t vec_rel = kVecRelNone;
      if (r.slot != UINT32_MAX) {
        store_.read_slot_row(t, t.slots[r.slot], rv);
        data = rv.data;
        len = rv.len;
        vec_rel = rv.vec_rel;
      } else {
        const OverlayRow& o = store_.overlay_for(t)->rows[r.overlay];
        data = o.enc.data();
        len = (uint32_t)o.enc.size();
        vec_rel = o.vec_rel;
      }
      Cand c;
      c.pk = r.pk;
      c.slot = r.slot;
      c.overlay = r.overlay;
      int32_t key_col = -1;
      if (order_item >= 0) {
        if (items[order_item].dist) {
          c.key.kind = 1;
          c.key.d = std::sqrt(l2_sq_double((const float*)(data + vec_rel), items[order_item].query.data(),
                                           (int)items[order_item].query.size()));
          push_candidate(std::move(c));
          continue;
        }
        key_col = items[order_item].col;
      } else if (order_col >= 0) {
        key_col = order_col;
      } else {
        c.key.kind = 0;
        c.key.i = r.pk;
        push_candidate(std::move(c));
        continue;
      }
      uint32_t n = 0;
      const char* p = column_ptr(schema, key_col, data, len, &n);
      if (!p) fail(ErrCode::Internal, "corrupt row");
      if (schema.cols[key_col].type == ColType::BigInt) {
        int64_t v = 0;
        std::memcpy(&v, p, 8);
        c.key.kind = 0;
        c.key.i = v;
      } else {
        c.key.kind = 2;
        c.key.s.assign(p, n);
      }
      push_candidate(std::move(c));
    }
  }

  std::vector<Cand>& sel = use_heap ? heap : all;
  std::sort(sel.begin(), sel.end(), [&](const Cand& a, const Cand& b) {
    int c = cmp_key(a.key, b.key);
    if (c != 0) return asc ? (c < 0) : (c > 0);
    return a.pk < b.pk;
  });
  if (limit >= 0 && (int64_t)sel.size() > limit) sel.resize((size_t)limit);

  rs.rows.reserve(sel.size());
  for (const Cand& c : sel) {
    const char* data;
    uint32_t len;
    uint32_t vec_rel = kVecRelNone;
    if (c.slot != UINT32_MAX) {
      store_.read_slot_row(t, t.slots[c.slot], rv);
      data = rv.data;
      len = rv.len;
      vec_rel = rv.vec_rel;
    } else {
      const OverlayRow& o = store_.overlay_for(t)->rows[c.overlay];
      data = o.enc.data();
      len = (uint32_t)o.enc.size();
      vec_rel = o.vec_rel;
    }
    std::vector<Cell> out;
    out.reserve(items.size());
    for (const ItemPlan& it : items) {
      if (it.hidden) continue;
      if (it.dist) {
        double d = std::sqrt(l2_sq_double((const float*)(data + vec_rel), it.query.data(), (int)it.query.size()));
        out.push_back(Cell::dbl(d));
        continue;
      }
      Value v = decode_column(schema, it.col, data, len);
      switch (v.type) {
        case ColType::BigInt:
          out.push_back(Cell::big(v.i));
          break;
        case ColType::VarChar:
          out.push_back(Cell::str(std::move(v.s)));
          break;
        case ColType::Vector:
          out.push_back(Cell::vec(std::move(v.v)));
          break;
      }
    }
    rs.rows.push_back(std::move(out));
  }
  return rs;
}

// ---------------------------------------------------------------- INSERT
ResultSet Executor::exec_insert(Stmt& stmt) {
  Table& t = store_.require_table(stmt.table);
  const Schema& schema = t.schema;
  std::vector<int32_t> target;
  if (stmt.insert_cols.empty()) {
    for (size_t i = 0; i < schema.cols.size(); ++i) target.push_back((int32_t)i);
  } else {
    std::vector<bool> seen(schema.cols.size(), false);
    for (const std::string& name : stmt.insert_cols) {
      int32_t ci = schema.find(name);
      if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + name);
      if (seen[ci]) fail(ErrCode::ColumnExists, "duplicate column in INSERT: " + name);
      seen[ci] = true;
      target.push_back(ci);
    }
  }
  std::vector<Row> rows;
  rows.reserve(stmt.insert_rows.size());
  for (const auto& raw : stmt.insert_rows) {
    if (raw.size() != target.size()) fail(ErrCode::InvalidValue, "column count does not match value count");
    Row row(schema.cols.size());
    std::vector<bool> got(schema.cols.size(), false);
    for (size_t i = 0; i < target.size(); ++i) {
      row[target[i]] = coerce_literal(schema.cols[target[i]], raw[i]);
      got[target[i]] = true;
    }
    for (size_t c = 0; c < schema.cols.size(); ++c) {
      if (!got[c]) fail(ErrCode::InvalidValue, "missing value for column " + schema.cols[c].name);
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty()) fail(ErrCode::SyntaxError, "INSERT without values");
  std::unordered_set<int64_t> seen;
  for (const Row& r : rows) {
    int64_t pk = r[schema.pk_idx].i;
    if (!seen.insert(pk).second) fail(ErrCode::DuplicateKey, "duplicate primary key in statement");
    if (store_.exists(t, pk)) fail(ErrCode::DuplicateKey, "duplicate primary key");
  }
  for (const Row& r : rows) store_.stage_put(t, r);
  if (!store_.in_txn()) store_.commit();
  ResultSet rs;
  rs.has_affected = true;
  rs.affected = (int64_t)rows.size();
  return rs;
}

// ---------------------------------------------------------------- UPDATE
ResultSet Executor::exec_update(Stmt& stmt) {
  Table& t = store_.require_table(stmt.table);
  const Schema& schema = t.schema;
  std::vector<std::pair<int32_t, Value>> sets;
  for (const auto& kv : stmt.sets) {
    int32_t ci = schema.find(kv.first);
    if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + kv.first);
    if (ci == schema.pk_idx) fail(ErrCode::UnsupportedSql, "primary key updates are not supported");
    sets.emplace_back(ci, coerce_literal(schema.cols[ci], kv.second));
  }
  if (sets.empty()) fail(ErrCode::SyntaxError, "UPDATE without SET");
  std::vector<CondPlan> conds;
  resolve_conditions(schema, stmt.where, conds);

  std::vector<RowRef> refs = collect_matching(t, conds);
  RowView rv;
  int64_t affected = 0;
  for (const RowRef& r : refs) {
    Row row;
    row.resize(schema.cols.size());
    if (r.slot != UINT32_MAX) {
      store_.read_slot_row(t, t.slots[r.slot], rv);
      for (size_t c = 0; c < schema.cols.size(); ++c) {
        row[c] = decode_column(schema, (int32_t)c, rv.data, rv.len);
      }
    } else {
      const OverlayRow& o = store_.overlay_for(t)->rows[r.overlay];
      for (size_t c = 0; c < schema.cols.size(); ++c) {
        row[c] = decode_column(schema, (int32_t)c, o.enc.data(), (uint32_t)o.enc.size());
      }
    }
    for (const auto& kv : sets) row[kv.first] = kv.second;
    store_.stage_put(t, row);
    ++affected;
  }
  if (!store_.in_txn()) store_.commit();
  ResultSet rs;
  rs.has_affected = true;
  rs.affected = affected;
  return rs;
}

// ---------------------------------------------------------------- DELETE
ResultSet Executor::exec_delete(Stmt& stmt) {
  Table& t = store_.require_table(stmt.table);
  const Schema& schema = t.schema;
  std::vector<CondPlan> conds;
  resolve_conditions(schema, stmt.where, conds);
  std::vector<RowRef> refs = collect_matching(t, conds);
  for (const RowRef& r : refs) store_.stage_delete(t, r.pk);
  if (!store_.in_txn()) store_.commit();
  ResultSet rs;
  rs.has_affected = true;
  rs.affected = (int64_t)refs.size();
  return rs;
}

// ---------------------------------------------------------------- dispatch
ResultSet Executor::execute(std::string_view sql) {
  Stmt stmt = parse_statement(sql);
  ResultSet rs;
  switch (stmt.kind) {
    case Stmt::Kind::Noop:
      return rs;
    case Stmt::Kind::Quit:
      rs.quit = true;
      return rs;
    case Stmt::Kind::Select:
      return exec_select(stmt);
    case Stmt::Kind::Insert:
      return exec_insert(stmt);
    case Stmt::Kind::Update:
      return exec_update(stmt);
    case Stmt::Kind::Delete:
      return exec_delete(stmt);
    case Stmt::Kind::CreateTable:
      store_.create_table(stmt.new_table, stmt.new_schema);
      return rs;
    case Stmt::Kind::CreateIndex:
      store_.create_vector_index(stmt.index_name, stmt.new_table, stmt.new_schema.cols[0].name);
      return rs;
    case Stmt::Kind::Begin:
      store_.begin();
      return rs;
    case Stmt::Kind::Commit:
      if (store_.in_txn()) store_.commit();
      return rs;
    case Stmt::Kind::Rollback:
      if (store_.in_txn()) store_.rollback();
      return rs;
    case Stmt::Kind::SetMode:
      ann_mode_ = stmt.ann_mode;
      return rs;
  }
  return rs;
}

}  // namespace mvd
