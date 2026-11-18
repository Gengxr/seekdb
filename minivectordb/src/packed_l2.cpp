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
  return codes_.capacity() + bias_.capacity() * sizeof(int32_t);
}

void PackedL2::clear() {
  std::vector<int8_t>().swap(codes_);
  std::vector<int32_t>().swap(bias_);
  valid_ = false;
}

bool PackedL2::build(const Table& table, int dim, size_t budget) {
  const size_t rows = (table.slots.size() + kTile - 1) / kTile * kTile;
  padded_dim_ = (static_cast<size_t>(dim) + 3) & ~size_t(3);
  if (rows > std::min(kMaxBytes, budget) / (padded_dim_ + 2 * sizeof(int32_t))) return false;
  double max_abs = 0;
  for (const Slot& s : table.slots) {
    if (s.dead()) continue;
    const char* v = table.arena.data() + s.arena_off + s.vec_rel;
    for (int j = 0; j < dim; ++j) {
      float x;
      std::memcpy(&x, v + 4 * j, sizeof x);
      max_abs = std::max(max_abs, std::fabs(static_cast<double>(x)));
    }
  }
  // Double grid arithmetic also handles subnormal and maximum finite floats.
  step_ = max_abs == 0 ? 1.0 : max_abs / 63.0;
  max_error_ = 0;
  codes_.assign(rows * padded_dim_, 0);
  bias_.assign(rows, kInvalid);
  for (size_t i = 0; i < table.slots.size(); ++i) {
    const Slot& s = table.slots[i];
    if (s.dead()) continue;
    const char* v = table.arena.data() + s.arena_off + s.vec_rel;
    int32_t b = 0;
    double error2 = 0;
    int8_t* tile = codes_.data() + (i / kTile) * kTile * padded_dim_;
    for (int j = 0; j < dim; ++j) {
      float x;
      std::memcpy(&x, v + 4 * j, sizeof x);
      const double scaled = static_cast<double>(x) / step_;
      const int r = static_cast<int>(std::round(scaled));
      tile[(j / 4) * kTile * 4 + (i % kTile) * 4 + (j % 4)] = static_cast<int8_t>(r);
      b += r * r + 256 * r;
      const double residual = scaled - r;
      error2 += residual * residual;
    }
    bias_[i] = b;
    max_error_ = std::max(max_error_, error2);
  }
  max_error_ = std::sqrt(max_error_);
  valid_ = true;
  return true;
}

bool PackedL2::candidates(const Table& table, const float* query, int dim, size_t k, size_t budget,
                          const std::vector<uint32_t>* matching,
                          std::vector<uint32_t>& result) {
#if defined(__x86_64__)
  static const bool disabled = std::getenv("MVD_NO_I8") != nullptr;
  if (disabled || !__builtin_cpu_supports("avx2") || dim < 8 || dim > 4096 ||
      table.live_rows < 256 || k == 0 || k > kTile) return false;
  if (matching && (matching->size() < k || matching->size() * 12 < table.slots.size()))
    return false;  // A short equality posting list is cheaper to scan directly.
  if (!valid_ && !build(table, dim, budget)) return false;

  static thread_local std::vector<uint32_t> words;
  words.assign(padded_dim_ / 4, 0x80808080u);
  auto* bytes = reinterpret_cast<uint8_t*>(words.data());
  int qnorm = 0;
  double error2 = 0;
  for (int j = 0; j < dim; ++j) {
    const double scaled = static_cast<double>(query[j]) / step_;
    if (std::fabs(scaled) > 126.0) return false;
    const int q = static_cast<int>(std::round(scaled));
    bytes[j] = static_cast<uint8_t>(q + 128);
    qnorm += q * q;
    const double residual = scaled - q;
    error2 += residual * residual;
  }

  static thread_local std::vector<int32_t> scores;
  scores.resize(bias_.size());
  std::array<int32_t, kTile> minima;
  static const auto scan = __builtin_cpu_supports("avxvnni") && !std::getenv("MVD_NO_VNNI")
                               ? scan_vnni : scan_avx2;
  scan(codes_.data(), bias_.data(), words.data(), words.size(), scores.size(), qnorm,
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
  }
  return true;
#else
  return false;
#endif
}

}  // namespace mvd
