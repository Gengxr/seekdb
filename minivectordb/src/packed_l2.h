#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvd {

struct Table;

// A derived index over committed, resident vectors. The integer distances only
// eliminate impossible hits. The caller orders survivors using original floats.
class PackedL2 {
 public:
  bool candidates(const Table& table, const float* query, int dim, size_t k, size_t budget,
                  const std::vector<uint32_t>* matching,
                  std::vector<uint32_t>& result);
  void invalidate() { valid_ = false; }
  bool ready() const { return valid_; }
  void clear();
  size_t bytes() const;

 private:
  bool build(const Table& table, int dim, size_t budget);
  std::vector<int8_t> codes_;
  std::vector<int32_t> bias_;
  double step_ = 0;
  double max_error_ = 0;  // Euclidean rounding error in grid units
  size_t padded_dim_ = 0;
  bool valid_ = false;
};

}  // namespace mvd
