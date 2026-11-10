#include "seekdb_cuvs_bridge.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

int main(int argc, char **argv)
{
  if (argc != 3) return 2;
  const long count = 10000;
  const long dimension = 32;
  std::vector<float> vectors(count * dimension);
  for (long row = 0; row < count; ++row) {
    for (long column = 0; column < dimension; ++column) {
      vectors[row * dimension + column] = std::sin(float(row * 31 + column * 17));
    }
  }
  void *index = nullptr;
  if (seekdb_cuvs_build(nullptr, count, dimension) != nullptr ||
      seekdb_cuvs_build(vectors.data(), count, 0) != nullptr ||
      seekdb_cuvs_build(vectors.data(), -1, dimension) != nullptr) return 15;
  if (std::strcmp(argv[1], "write") == 0) {
    index = seekdb_cuvs_build(vectors.data(), count, dimension);
    if (!index || seekdb_cuvs_serialize(index, argv[2]) != 0) return 3;
  } else {
    void *invalid = seekdb_cuvs_deserialize(argv[2], count - 1, dimension);
    if (invalid) { seekdb_cuvs_free(invalid); return 6; }
    invalid = seekdb_cuvs_deserialize(argv[2], count, dimension + 1);
    if (invalid) { seekdb_cuvs_free(invalid); return 7; }
    index = seekdb_cuvs_deserialize(argv[2], count, dimension);
    if (!index) return 4;
  }
  unsigned int ids[10];
  float distances[10];
  int result = seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, distances);
  if (result != 0 || ids[0] != 0 || std::fabs(distances[0]) > 0.0001f) return 5;
  // Exercise cached workspace growth, reuse, the uncached batch path, and
  // returning to a smaller query after a batch (also after snapshot reload).
  const long ks[] = {10, 100, 10, 100};
  for (long k : ks) {
    for (long nq : {1L, 2L, 1L}) {
      std::vector<unsigned int> neighbors(nq * k);
      std::vector<float> distances(nq * k);
      if (seekdb_cuvs_search(index, vectors.data(), nq, k,
                             neighbors.data(), distances.data()) != 0) return 8;
      for (long q = 0; q < nq; ++q) {
        if (neighbors[q * k] != (unsigned)q || std::fabs(distances[q * k]) > 0.0001f) return 9;
        for (long i = 0; i < k; ++i) {
          if (neighbors[q * k + i] >= count) return 10;
          for (long j = 0; j < i; ++j) {
            if (neighbors[q * k + i] == neighbors[q * k + j]) return 11;
          }
        }
      }
      if (seekdb_cuvs_search(index, vectors.data(), nq, k,
                             neighbors.data(), nullptr) != 0) return 12;
    }
  }
  if (seekdb_cuvs_search(index, vectors.data(), 0, 10, ids, distances) == 0) return 13;
  // A rejected request must not poison or leave the workspace locked.
  if (seekdb_cuvs_search(index, vectors.data(), 1, 10, ids, distances) != 0) return 14;
  seekdb_cuvs_free(index);
  std::printf("%s: n=%ld top1=%u distance=%g\n", argv[1], count, ids[0], distances[0]);
  return 0;
}
