#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace mvd {

struct Table;

// A derived index over committed, resident vectors. The integer distances only
// eliminate impossible hits. The caller orders survivors using original floats.
class PackedL2 {
 public:
  bool candidates(const Table& table, const float* query, int dim, size_t k, size_t budget,
                  const std::vector<uint32_t>* matching, int filter_column,
                  std::vector<uint32_t>& result);
  void invalidate() { valid_ = false; seen_column_ = tried_column_ = -1; }
  bool ready() const { return valid_; }
  void clear();
  size_t bytes() const;

 private:
  bool build(const Table& table, int dim, size_t budget);
  void group_by_column(const Table& table, int column, size_t budget);
  struct TileRange { size_t begin, count; };
  std::unordered_map<uint32_t, TileRange> ranges_;
  std::vector<int8_t> grouped_codes_;
  std::vector<int32_t> grouped_bias_;
  std::vector<uint32_t> slot_ids_;
  void clear_groups();
  std::vector<int8_t> codes_;
  std::vector<int32_t> bias_;
  double inv_step_ = 0;
  double max_error_ = 0;  // Euclidean rounding error in grid units
  size_t padded_dim_ = 0;
  int grouped_column_ = -1, seen_column_ = -1, tried_column_ = -1;
  bool valid_ = false;
};

}  // namespace mvd
