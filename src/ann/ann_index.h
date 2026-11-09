#pragma once

// Self-contained HNSW approximate-nearest-neighbour index.
//
// This file is deliberately dependency-free (C++20 standard library only) so
// that it can be dropped into the executor later without pulling in a
// third-party ANN library.  The index owns a dense copy of the vectors; row i
// of `build()` data is internal id i.
//
// Metric: squared L2 (the ordering is identical to L2, but no sqrt is taken).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mvdb::ann {

// Squared L2 distance.  Uses AVX2+FMA when the translation unit is compiled
// with those features enabled, with a portable scalar fallback otherwise.
float l2_squared(const float* a, const float* b, int dim);

class AnnIndex {
 public:
  // `M` is the target number of neighbours per node (layer 0 uses 2*M).
  // `ef_construction` is the candidate-list width while inserting.
  // `seed` makes level generation and all tie-breaking deterministic.
  AnnIndex(int dim, int M = 16, int ef_construction = 200,
           uint64_t seed = 42);
  ~AnnIndex();

  AnnIndex(const AnnIndex&) = delete;
  AnnIndex& operator=(const AnnIndex&) = delete;
  AnnIndex(AnnIndex&&) noexcept;
  AnnIndex& operator=(AnnIndex&&) noexcept;

  // Replaces the whole index.  data[i] must have exactly `dim()` floats.
  // Internal ids are dense: 0 .. data.size()-1.
  void build(const std::vector<std::vector<float>>& data);

  // Returns at most k internal ids, nearest first.  `ef_search` is clamped to
  // at least k.  Safe to call concurrently after build() has finished.
  std::vector<uint32_t> search(const float* query, size_t k,
                               size_t ef_search) const;

  size_t size() const;
  int dim() const;
  int max_level() const;
  int entry_point() const;

  // Approximate heap footprint of the owned vectors and adjacency lists.
  // Allocator bookkeeping and the AnnIndex object itself are not included.
  size_t memoryBytes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mvdb::ann
