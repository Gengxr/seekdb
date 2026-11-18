#include "packed_l2.h"
#include "storage.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace mvd {
namespace {

// Four adjacent dimensions per row, 32 rows per tile. Each SIMD lane therefore
// accumulates the dot product for one row, with no horizontal reductions.
constexpr size_t kTile = 32;
constexpr int32_t kInvalid = 1 << 29;
constexpr size_t kMaxBytes = 32u << 20;

#if defined(__x86_64__)
__attribute__((target("avx2"), always_inline))
inline __m256i dot8_avx2(__m256i acc, __m256i q, __m256i x) {
  // x is in [-63,63], so the signed pairwise sum cannot saturate int16.
  return _mm256_add_epi32(acc, _mm256_madd_epi16(_mm256_maddubs_epi16(q, x),
                                                  _mm256_set1_epi16(1)));
}
__attribute__((target("avx2,avxvnni"), always_inline))
inline __m256i dot8_vnni(__m256i acc, __m256i q, __m256i x) {
  return _mm256_dpbusd_avx_epi32(acc, q, x);
}

// Instantiate the identical tile traversal for each instruction set; dispatch
// once per query. Applying a target attribute only to a callback inside this
// loop would turn each four-byte dot product into a function call.
#define MVD_PACKED_SCAN(NAME, ISA, DOT) \
  __attribute__((target(ISA))) \
  void NAME(const int8_t* codes, const int32_t* bias, const uint32_t* query, \
            size_t groups, size_t rows, int qnorm, int32_t* scores, int32_t* minima) { \
    __m256i best[4]; \
    for (auto& b : best) b = _mm256_set1_epi32(kInvalid); \
    for (size_t i = 0; i < rows; i += kTile) { \
      __m256i acc[4]{}; \
      for (size_t j = 0; j < groups; ++j, codes += kTile * 4) { \
        const __m256i q = _mm256_set1_epi32(static_cast<int>(query[j])); \
        for (int v = 0; v < 4; ++v) \
          acc[v] = DOT(acc[v], q, _mm256_loadu_si256((const __m256i*)(codes + v * 32))); \
      } \
      for (int v = 0; v < 4; ++v) { \
        const __m256i b = _mm256_loadu_si256((const __m256i*)(bias + i + 8 * v)); \
        const __m256i d = _mm256_sub_epi32(_mm256_add_epi32(b, _mm256_set1_epi32(qnorm)), \
                                           _mm256_slli_epi32(acc[v], 1)); \
        _mm256_storeu_si256((__m256i*)(scores + i + 8 * v), d); \
        best[v] = _mm256_min_epi32(best[v], d); \
      } \
    } \
    for (int v = 0; v < 4; ++v) _mm256_storeu_si256((__m256i*)(minima + 8 * v), best[v]); \
  }

MVD_PACKED_SCAN(scan_avx2, "avx2", dot8_avx2)
MVD_PACKED_SCAN(scan_vnni, "avx2,avxvnni", dot8_vnni)
#undef MVD_PACKED_SCAN

__attribute__((target("avx2")))
float row_max_abs(const char* v, int dim) {
  const __m256 sign = _mm256_set1_ps(-0.0f);
  __m256 vmax = _mm256_setzero_ps();
  int j = 0;
  for (; j + 8 <= dim; j += 8) {
    const auto x = _mm256_loadu_ps(reinterpret_cast<const float*>(v + 4 * j));
    vmax = _mm256_max_ps(vmax, _mm256_andnot_ps(sign, x));
  }
  alignas(32) float lanes[8];
  _mm256_store_ps(lanes, vmax);
  float best = *std::max_element(lanes, lanes + 8);
  for (; j < dim; ++j) {
    float x;
    std::memcpy(&x, v + 4 * j, sizeof x);
    best = std::max(best, std::fabs(x));
  }
  return best;
}

// Write the four dimension bytes of a row directly into their tile. Rounding
// ties to either nearest integer is valid because we measure the actual error.
__attribute__((target("avx2")))
double pack_row(const char* v, int dim, double inv_step, int8_t* dest, int32_t& bias) {
  const auto inv = _mm256_set1_pd(inv_step);
  __m256d errors = _mm256_setzero_pd();
  __m128i norms = _mm_setzero_si128();
  int j = 0;
  for (; j + 4 <= dim; j += 4) {
    auto x = _mm_loadu_ps(reinterpret_cast<const float*>(v + 4 * j));
    auto scaled = _mm256_mul_pd(_mm256_cvtps_pd(x), inv);
    auto grid = _mm256_round_pd(scaled, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    auto residual = _mm256_sub_pd(scaled, grid);
    errors = _mm256_add_pd(errors, _mm256_mul_pd(residual, residual));
    auto q = _mm256_cvttpd_epi32(grid);
    norms = _mm_add_epi32(norms, _mm_add_epi32(_mm_mullo_epi32(q, q), _mm_slli_epi32(q, 8)));
    auto words = _mm_packs_epi32(q, q);
    int bytes = _mm_cvtsi128_si32(_mm_packs_epi16(words, words));
    std::memcpy(dest + (j / 4) * kTile * 4, &bytes, 4);
  }
  alignas(32) double err[4];
  alignas(16) int32_t b[4];
  _mm256_store_pd(err, errors);
  _mm_store_si128(reinterpret_cast<__m128i*>(b), norms);
  bias = b[0] + b[1] + b[2] + b[3];
  double error2 = (err[0] + err[1]) + (err[2] + err[3]);
  for (int lane = 0; j < dim; ++j, ++lane) {
    float x;
    std::memcpy(&x, v + 4 * j, sizeof x);
    const double scaled = x * inv_step;
    const int q = static_cast<int>(std::round(scaled));
    dest[(j / 4) * kTile * 4 + lane] = static_cast<int8_t>(q);
    bias += q * q + 256 * q;
    const double residual = scaled - q;
    error2 += residual * residual;
  }
  return error2;
}

__attribute__((target("avx2")))
void collect_within(const int32_t* scores, size_t rows, int32_t limit,
                    std::vector<uint32_t>& result) {
  const __m256i exclusive = _mm256_set1_epi32(limit + 1);
  for (size_t i = 0; i < rows; i += 8) {
    __m256i s = _mm256_loadu_si256((const __m256i*)(scores + i));
    unsigned mask = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(exclusive, s)));
    while (mask) {
      result.push_back(static_cast<uint32_t>(i + __builtin_ctz(mask)));
      mask &= mask - 1;
    }
  }
}
#endif

}  // namespace

size_t PackedL2::bytes() const {
  return codes_.capacity() + grouped_codes_.capacity() +
         (bias_.capacity() + grouped_bias_.capacity() + slot_ids_.capacity()) * sizeof(uint32_t) +
         ranges_.bucket_count() * sizeof(void*) + ranges_.size() * (sizeof(TileRange) + 3 * sizeof(void*));
}

void PackedL2::clear_groups() {
  std::vector<int8_t>().swap(grouped_codes_);
  std::vector<int32_t>().swap(grouped_bias_);
  std::vector<uint32_t>().swap(slot_ids_);
  decltype(ranges_)().swap(ranges_);
  grouped_column_ = -1;
}

void PackedL2::clear() {
  std::vector<int8_t>().swap(codes_);
  std::vector<int32_t>().swap(bias_);
  clear_groups();
  invalidate();
}

// A committed row can keep its tile when the original grid still contains
// it. The certified maximum rounding error may grow. Reassigning the grid or
// adding another tile requires a rebuild from all current rows.
void PackedL2::update_slot(const Table& table, uint32_t sid) {
#if defined(__x86_64__)
  if (!valid_) return;
  if (sid >= bias_.size() || !table.cached) {
    invalidate();
    return;
  }
  if (grouped_column_ >= 0) clear_groups();
  seen_column_ = tried_column_ = -1;
  const Slot& s = table.slots[sid];
  int8_t* dest = codes_.data() + (sid / kTile) * kTile * padded_dim_ + (sid % kTile) * 4;
  if (s.dead()) {
    for (size_t j = 0; j < padded_dim_; j += 4) std::memset(dest + j * kTile, 0, 4);
    bias_[sid] = kInvalid;
    return;
  }
  const int dim = table.schema.cols[table.vec_idx()].len;
  const char* v = table.arena.data() + s.arena_off + s.vec_rel;
  if (row_max_abs(v, dim) * inv_step_ > 63.0 + 1e-12) {
    invalidate();
    return;
  }
  const double error2 = pack_row(v, dim, inv_step_, dest, bias_[sid]);
  max_error_ = std::max(max_error_, std::sqrt(error2));
#endif
}

bool PackedL2::build(const Table& table, int dim, size_t budget) {
#if defined(__x86_64__)
  const size_t rows = (table.slots.size() + kTile - 1) / kTile * kTile;
  padded_dim_ = (static_cast<size_t>(dim) + 3) & ~size_t(3);
  if (rows > std::min(kMaxBytes, budget) / (padded_dim_ + 2 * sizeof(int32_t))) return false;
  if (grouped_column_ >= 0) clear_groups();
  float max_abs = 0;
  for (const Slot& s : table.slots) {
    if (s.dead()) continue;
    const char* v = table.arena.data() + s.arena_off + s.vec_rel;
    max_abs = std::max(max_abs, row_max_abs(v, dim));
  }
  // Double grid arithmetic also handles subnormal and maximum finite floats.
  inv_step_ = max_abs == 0 ? 1.0 : 63.0 / static_cast<double>(max_abs);
  max_error_ = 0;
  codes_.assign(rows * padded_dim_, 0);
  bias_.assign(rows, kInvalid);
  for (size_t i = 0; i < table.slots.size(); ++i) {
    const Slot& s = table.slots[i];
    if (s.dead()) continue;
    const char* v = table.arena.data() + s.arena_off + s.vec_rel;
    int8_t* dest = codes_.data() + (i / kTile) * kTile * padded_dim_ + (i % kTile) * 4;
    double error2 = pack_row(v, dim, inv_step_, dest, bias_[i]);
    max_error_ = std::max(max_error_, error2);
  }
  max_error_ = std::sqrt(max_error_);
  valid_ = true;
  return true;
#else
  return false;
#endif
}

// After repeated use of a low-cardinality equality column, put its postings
// in contiguous tiles. A filtered query then reads just its own range. Plain
// scans keep their original physical order and do not access the extra copy.
void PackedL2::group_by_column(const Table& table, int column, size_t budget) {
  const auto& index = table.eq.at(static_cast<size_t>(column));
  const size_t groups = index.by_int.size() + index.by_str.size();
  if (!index.built || index.disabled || groups < 2 || groups > table.live_rows / 192) return;
  auto align_tile = [](size_t n) { return (n + kTile - 1) / kTile * kTile; };
  size_t padded = 0;
  for (const auto& entry : index.by_int) padded += align_tile(entry.second.size());
  for (const auto& entry : index.by_str) padded += align_tile(entry.second.size());
  const size_t cap = std::min(kMaxBytes, budget);
  const size_t existing = bytes() + groups * 64;
  if (existing >= cap || padded > (cap - existing) / (padded_dim_ + 3 * sizeof(int32_t))) return;
  std::vector<int8_t> codes(padded * padded_dim_, 0);
  std::vector<int32_t> bias(padded, kInvalid);
  std::vector<uint32_t> slots(padded);
  decltype(ranges_) ranges;
  ranges.reserve(groups);
  size_t begin = 0, live = 0;
  auto pack_postings = [&](const auto& buckets) {
    for (const auto& entry : buckets) {
      const auto& ids = entry.second;
      if (ids.empty()) continue;
      const size_t count = align_tile(ids.size());
      ranges.emplace(ids.front(), TileRange{begin, count});
      for (size_t j = 0; j < ids.size(); ++j) {
        const size_t sid = ids[j], pos = begin + j;
        if (table.slots[sid].dead()) continue;
        ++live;
        slots[pos] = static_cast<uint32_t>(sid);
        bias[pos] = bias_[sid];
        const size_t src = (sid / kTile) * kTile * padded_dim_ + (sid % kTile) * 4;
        const size_t dst = (pos / kTile) * kTile * padded_dim_ + (pos % kTile) * 4;
        for (size_t d = 0; d < padded_dim_; d += 4)
          std::memcpy(codes.data() + dst + d * kTile, codes_.data() + src + d * kTile, 4);
      }
      begin += count;
    }
  };
  pack_postings(index.by_int);
  pack_postings(index.by_str);
  if (live != table.live_rows) return;
  grouped_codes_.swap(codes);
  grouped_bias_.swap(bias);
  slot_ids_.swap(slots);
  ranges_.swap(ranges);
  grouped_column_ = column;
}

bool PackedL2::candidates(const Table& table, const float* query, int dim, size_t k, size_t budget,
                          const std::vector<uint32_t>* matching, int filter_column,
                          std::vector<uint32_t>& result) {
#if defined(__x86_64__)
  static const bool disabled = std::getenv("MVD_NO_I8") != nullptr;
  if (disabled || !__builtin_cpu_supports("avx2") || dim < 8 || dim > 4096 ||
      table.live_rows < 256 || k == 0 || k > kTile) return false;
  if (matching && matching->size() < k) return false;
  const bool repeated = matching && seen_column_ == filter_column && tried_column_ != filter_column;
  if (matching) seen_column_ = filter_column;
  const bool sparse = matching && matching->size() * 12 < table.slots.size();
  if (!valid_ && sparse && !repeated) return false;
  if (!valid_ && !build(table, dim, budget)) return false;
  if (repeated && grouped_column_ < 0) {
    tried_column_ = filter_column;
    group_by_column(table, filter_column, budget);
  }
  TileRange range{0, bias_.size()};
  const bool use_group = matching && filter_column == grouped_column_;
  if (use_group) {
    const auto it = ranges_.find(matching->front());
    if (it == ranges_.end()) return false;
    range = it->second;
    matching = nullptr;  // Every live row in this range already satisfies the predicate.
  } else if (sparse) {
    return false;
  }

  static thread_local std::vector<uint32_t> words;
  words.assign(padded_dim_ / 4, 0x80808080u);
  auto* bytes = reinterpret_cast<uint8_t*>(words.data());
  int qnorm = 0;
  double error2 = 0;
  for (int j = 0; j < dim; ++j) {
    const double scaled = static_cast<double>(query[j]) * inv_step_;
    if (std::fabs(scaled) > 126.0) return false;
    const int q = static_cast<int>(std::round(scaled));
    bytes[j] = static_cast<uint8_t>(q + 128);
    qnorm += q * q;
    const double residual = scaled - q;
    error2 += residual * residual;
  }

  static thread_local std::vector<int32_t> scores;
  scores.resize(range.count);
  std::array<int32_t, kTile> minima;
  static const auto scan = __builtin_cpu_supports("avxvnni") && !std::getenv("MVD_NO_VNNI")
                               ? scan_vnni : scan_avx2;
  const auto& codes = use_group ? grouped_codes_ : codes_;
  const auto& bias = use_group ? grouped_bias_ : bias_;
  scan(codes.data() + range.begin * padded_dim_, bias.data() + range.begin,
       words.data(), words.size(), scores.size(), qnorm,
       scores.data(), minima.data());
  if (matching) {
    minima.fill(kInvalid);
    for (size_t i = 0; i < matching->size(); ++i) {
      int32_t& best = minima[i % kTile];
      best = std::min(best, scores[(*matching)[i]]);
    }
  }
  // The k smallest lane minima belong to distinct rows. Their largest integer
  // distance gives a valid upper bound even when several nearest hits share a lane.
  std::nth_element(minima.begin(), minima.begin() + (k - 1), minima.end());
  const int32_t upper = minima[k - 1];
  if (upper == kInvalid) return false;
  // Each grid distance is within e of its true distance in grid units. A row
  // further than upper_radius+2e cannot beat the k witnessed live rows. Round
  // outwards and leave slack for double operations before taking the square.
  const double error = max_error_ + std::sqrt(error2);
  const double radius = (std::sqrt(static_cast<double>(upper)) + 2 * error) * (1 + 1e-12) + 1e-9;
  const int32_t limit = static_cast<int32_t>(std::min<double>(kInvalid - 1, std::ceil(radius * radius)));
  result.clear();
  if (matching) {
    for (uint32_t i : *matching)
      if (scores[i] <= limit) result.push_back(i);
  } else {
    collect_within(scores.data(), scores.size(), limit, result);
    if (use_group)
      for (auto& i : result) i = slot_ids_[range.begin + i];
  }
  return true;
#else
  return false;
#endif
}

}  // namespace mvd
