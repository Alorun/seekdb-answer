#pragma once

// Approximate TopK over the existing 7-bit (q7) row codes.
//
// DESIGN
//   Two-stage candidate generation, one exact re-rank:
//
//   1. Candidates come from the *existing* Quant8Cache codes (1 byte per
//      dimension, already built by the exact path), so the index adds no
//      second copy of the code matrix and reuses the exact-mode encoders.
//      Two interchangeable candidate generators:
//        * Clusterable tables  -> IVF: k-means lists, the nprobe nearest
//          lists are scanned (this is where the large win is).
//        * Everything else     -> one flat pass over the table.  The q7 code
//          distance is accurate enough that the exact top-10 is always inside
//          the 16-24 best q7 candidates (measured: rank <= 13 of 10000), so
//          this pass never needs a triangle-inequality bound and never has to
//          evaluate float distances mid-scan.
//   2. The candidate pool is re-ranked with the exact float32 squared-L2
//      kernel, ties broken by ascending primary key, so returned distances are
//      exact and ids can never repeat.
//
//   The candidate stage keeps a *pool* of the smallest code distances using
//   append-only storage plus periodic compaction (no per-row heap, no O(pool)
//   shifting), which keeps the scan kernel within ~10% of a raw q7 dot pass.
//
//   `exact` mode must keep using scanQ7ExactRange; this module is the `ann`
//   mode candidate path only.
//
// THREAD SAFETY
//   build() must be externally serialised.  search() is const and takes all
//   mutable state from the caller-supplied Scratch, so concurrent searches
//   after a build are safe.  Determinism: no randomness in queries; k-means
//   init is deterministic and row order is the caller's pk order.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mvdb::annq {

struct Hit {
  double d = 0;
  int64_t pk = 0;
  size_t vi = 0;
};

// Non-owning view of the caller's row store + q7 cache.
struct Rows {
  const uint8_t* codes = nullptr;      // n * dim biased codes, b = code + 63
  const int32_t* pv = nullptr;         // n row constants
  const float* const* vecp = nullptr;  // n pointers to the float rows
  const int64_t* pks = nullptr;        // n primary keys
  size_t n = 0;
  size_t dim = 0;
  float scale = 1.0f;                  // q7 scale (unused by the scan, kept
                                       // so callers can assert consistency)
};

struct Options {
  // Candidates handed to the exact re-rank.  24 is the safe default: the q7
  // code ranking puts the exact top-10 within rank 13 (worst observed over
  // 1000 uniform + 1000 clustered queries), so 24 leaves 11 rows of headroom.
  size_t pool = 24;
  // IVF parameters (only used when the table is classified as clusterable).
  size_t nlist = 64;
  size_t nprobe = 6;
  int kmeans_iters = 6;
  // Keep IVF only when k-means removes enough variance; uniform data in 128
  // dimensions barely clusters and IVF then degrades to a random subset, which
  // would silently lose recall.  Gate on data, not on a flag.
  double ivf_max_inertia_ratio = 0.45;
  // Do not build IVF below this row count (lists would be tiny).
  size_t ivf_min_rows = 2000;
};

class Q7AnnIndex {
 public:
  Q7AnnIndex() = default;

  // Replaces the index.  Does not copy the float rows or the q7 codes.
  void build(const Rows& rows, const Options& opt = Options());
  void clear();

  // Returns the exact top-k (`k` <= pool) of the candidate pool, nearest
  // first, distances exact float32 squared L2, ties by ascending pk.
  void search(const Rows& rows, const float* q, const int8_t* qcodes,
              int32_t cq, size_t k, std::vector<Hit>& out,
              struct Scratch& scratch) const;

  bool clustered() const { return clustered_; }
  size_t nlist() const { return nlist_; }
  size_t nprobe() const { return nprobe_; }
  size_t pool() const { return opt_.pool; }
  // Bytes owned by the index (coarse structure + list layout).  Excludes the
  // caller-owned code matrix and float rows.
  size_t bytes() const;

 private:
  Options opt_{};
  size_t n_ = 0, dim_ = 0;
  bool clustered_ = false;
  size_t nlist_ = 0, nprobe_ = 0;
  // IVF state (empty when clustered_ == false).
  std::vector<float> cent_;      // nlist * dim
  std::vector<uint8_t> ccode_;   // nlist * dim biased q7 codes of centroids
  std::vector<int32_t> cpv_;     // nlist
  std::vector<uint32_t> order_;  // rows grouped by list
  std::vector<uint32_t> off_;    // nlist + 1
};

// Per-search scratch, sized by prepare().  Reuse across queries.
struct Scratch {
  std::vector<int32_t> cand_v;
  std::vector<uint32_t> cand_i;
  std::vector<uint32_t> order_cent;  // nlist
  std::vector<float> qtmp;

  void prepare(size_t n, size_t nlist, size_t pool) {
    cand_v.reserve(pool * 8 + 64);
    cand_i.reserve(pool * 8 + 64);
    cand_v.resize(0);
    cand_i.resize(0);
    order_cent.resize(nlist);
    (void)n;
  }
};

// ---------------------------------------------------------------------------
// Build statistics, for the M/B budget report.
struct BuildStats {
  double ms = 0;
  size_t bytes = 0;
  bool clustered = false;
  size_t nlist = 0;
  size_t nprobe = 0;
};

Q7AnnIndex* ensureIndex(void*& slot, const Rows& rows, const Options& opt,
                        BuildStats* stats);

}  // namespace mvdb::annq
