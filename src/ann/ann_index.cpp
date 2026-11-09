#include "ann_index.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

#if defined(__AVX2__) && defined(__FMA__)
#include <immintrin.h>
#endif

namespace mvdb::ann {

float l2_squared(const float* a, const float* b, int dim) {
  if (dim <= 0) return 0.0f;
#if defined(__AVX2__) && defined(__FMA__)
  const float* __restrict pa = a;
  const float* __restrict pb = b;
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  __m256 acc2 = _mm256_setzero_ps();
  __m256 acc3 = _mm256_setzero_ps();
  int i = 0;
  for (; i + 32 <= dim; i += 32) {
    const __m256 d0 =
        _mm256_sub_ps(_mm256_loadu_ps(pa + i), _mm256_loadu_ps(pb + i));
    const __m256 d1 = _mm256_sub_ps(_mm256_loadu_ps(pa + i + 8),
                                    _mm256_loadu_ps(pb + i + 8));
    const __m256 d2 = _mm256_sub_ps(_mm256_loadu_ps(pa + i + 16),
                                    _mm256_loadu_ps(pb + i + 16));
    const __m256 d3 = _mm256_sub_ps(_mm256_loadu_ps(pa + i + 24),
                                    _mm256_loadu_ps(pb + i + 24));
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
    acc2 = _mm256_fmadd_ps(d2, d2, acc2);
    acc3 = _mm256_fmadd_ps(d3, d3, acc3);
  }
  for (; i + 8 <= dim; i += 8) {
    const __m256 d =
        _mm256_sub_ps(_mm256_loadu_ps(pa + i), _mm256_loadu_ps(pb + i));
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }
  __m256 acc = _mm256_add_ps(_mm256_add_ps(acc0, acc1),
                             _mm256_add_ps(acc2, acc3));
  __m128 sum = _mm_add_ps(_mm256_castps256_ps128(acc),
                          _mm256_extractf128_ps(acc, 1));
  sum = _mm_hadd_ps(sum, sum);
  sum = _mm_hadd_ps(sum, sum);
  float result = _mm_cvtss_f32(sum);
  for (; i < dim; ++i) {
    const float d = pa[i] - pb[i];
    result += d * d;
  }
  return result;
#else
  float result = 0.0f;
  for (int i = 0; i < dim; ++i) {
    const float d = a[i] - b[i];
    result += d * d;
  }
  return result;
#endif
}

namespace {

constexpr int kMaxLevel = 32;

struct Neighbor {
  float dist;
  uint32_t id;
};

// std::*_heap comparator: min-heap ordered by (distance, id).
struct MinByDistance {
  bool operator()(const Neighbor& a, const Neighbor& b) const {
    if (a.dist != b.dist) return a.dist > b.dist;
    return a.id > b.id;
  }
};

// std::*_heap comparator: max-heap ordered by (distance, id).
struct MaxByDistance {
  bool operator()(const Neighbor& a, const Neighbor& b) const {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.id < b.id;
  }
};

struct Scratch {
  std::vector<uint32_t> visited;
  uint32_t epoch = 0;

  void begin(size_t n) {
    if (visited.size() != n) visited.assign(n, 0);
    if (++epoch == 0) {
      std::fill(visited.begin(), visited.end(), 0);
      epoch = 1;
    }
  }
};

struct Node {
  int level = 0;
  std::vector<std::vector<uint32_t>> links;  // links[level]
};

}  // namespace

struct AnnIndex::Impl {
  int dim = 0;
  int M = 16;
  int M0 = 32;
  int ef_construction = 200;
  uint64_t seed = 42;
  uint64_t rng_state = 0;

  std::vector<float> data;  // dense row-major copy
  std::vector<Node> nodes;
  int entry = -1;
  int top_level = -1;

  // Reused only during build(); search() uses a per-call scratch so that
  // concurrent reads do not race.
  Scratch build_scratch;

  Impl(int d, int m, int efc, uint64_t s)
      : dim(d),
        M(m),
        M0(2 * m),
        ef_construction(efc),
        seed(s),
        rng_state(s) {}

  uint64_t nextRandom() {
    // SplitMix64: tiny, deterministic, and independent of the standard
    // library implementation.
    uint64_t z = (rng_state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  double uniform01() {
    // 53 random mantissa bits in [0, 1).
    return static_cast<double>(nextRandom() >> 11) * (1.0 / 9007199254740992.0);
  }

  int randomLevel() {
    double u = uniform01();
    if (u < 1e-15) u = 1e-15;
    const double scale = 1.0 / std::log(static_cast<double>(M));
    int level = static_cast<int>(-std::log(u) * scale);
    if (level < 0) level = 0;
    if (level > kMaxLevel) level = kMaxLevel;
    return level;
  }

  const float* row(uint32_t id) const {
    return data.data() + static_cast<size_t>(id) * static_cast<size_t>(dim);
  }

  size_t maxConnections(int level) const {
    return level == 0 ? static_cast<size_t>(M0) : static_cast<size_t>(M);
  }

  uint32_t greedySearch(const float* query, uint32_t ep, int level) const {
    float best = l2_squared(query, row(ep), dim);
    bool improved = true;
    while (improved) {
      improved = false;
      const auto& neighbors = nodes[ep].links[static_cast<size_t>(level)];
      for (uint32_t candidate : neighbors) {
        const float d = l2_squared(query, row(candidate), dim);
        if (d < best) {
          best = d;
          ep = candidate;
          improved = true;
        }
      }
    }
    return ep;
  }

  // Best-first search of one layer.  Returns the found nodes sorted by
  // (distance, id) ascending.  `scratch` is not shared across threads.
  std::vector<Neighbor> searchLayer(const float* query, uint32_t ep, size_t ef,
                                    int level, Scratch& scratch) const {
    if (ef < 1) ef = 1;
    scratch.begin(nodes.size());

    std::vector<Neighbor> candidates;
    std::vector<Neighbor> results;
    candidates.reserve(ef * 2 + 8);
    results.reserve(ef + 1);

    const float d0 = l2_squared(query, row(ep), dim);
    scratch.visited[ep] = scratch.epoch;
    candidates.push_back({d0, ep});
    std::push_heap(candidates.begin(), candidates.end(), MinByDistance{});
    results.push_back({d0, ep});
    std::push_heap(results.begin(), results.end(), MaxByDistance{});

    while (!candidates.empty()) {
      const Neighbor current = candidates.front();
      if (results.size() >= ef && current.dist > results.front().dist) break;
      std::pop_heap(candidates.begin(), candidates.end(), MinByDistance{});
      candidates.pop_back();

      if (nodes[current.id].links.size() <= static_cast<size_t>(level)) continue;
      const auto& neighbors = nodes[current.id].links[static_cast<size_t>(level)];
      for (uint32_t candidate : neighbors) {
        if (scratch.visited[candidate] == scratch.epoch) continue;
        scratch.visited[candidate] = scratch.epoch;
        const float d = l2_squared(query, row(candidate), dim);
        if (results.size() < ef || d < results.front().dist) {
          candidates.push_back({d, candidate});
          std::push_heap(candidates.begin(), candidates.end(),
                         MinByDistance{});
          results.push_back({d, candidate});
          std::push_heap(results.begin(), results.end(), MaxByDistance{});
          if (results.size() > ef) {
            std::pop_heap(results.begin(), results.end(), MaxByDistance{});
            results.pop_back();
          }
        }
      }
    }

    std::vector<Neighbor> out;
    out.reserve(results.size());
    while (!results.empty()) {
      out.push_back(results.front());
      std::pop_heap(results.begin(), results.end(), MaxByDistance{});
      results.pop_back();
    }
    std::reverse(out.begin(), out.end());
    return out;
  }

  // HNSW neighbour-selection heuristic, with nearest-first filling if the
  // diversity test selects fewer than `limit` nodes.  `candidates` is sorted
  // by (distance to qid, id) ascending.
  std::vector<uint32_t> selectNeighbors(uint32_t qid,
                                        const std::vector<Neighbor>& candidates,
                                        size_t limit) const {
    std::vector<uint32_t> selected;
    std::vector<uint32_t> rejected;
    selected.reserve(limit);
    rejected.reserve(candidates.size());
    for (const Neighbor& candidate : candidates) {
      if (candidate.id == qid) continue;
      bool keep = true;
      const float* c = row(candidate.id);
      for (uint32_t chosen : selected) {
        if (l2_squared(c, row(chosen), dim) < candidate.dist) {
          keep = false;
          break;
        }
      }
      if (keep) {
        selected.push_back(candidate.id);
        if (selected.size() == limit) return selected;
      } else {
        rejected.push_back(candidate.id);
      }
    }
    // Keep the graph well connected even when the heuristic prunes heavily.
    for (uint32_t id : rejected) {
      if (selected.size() == limit) break;
      selected.push_back(id);
    }
    return selected;
  }

  std::vector<uint32_t> sortedNeighbors(uint32_t id,
                                        const std::vector<uint32_t>& ids,
                                        size_t limit) const {
    std::vector<Neighbor> candidates;
    candidates.reserve(ids.size());
    const float* q = row(id);
    for (uint32_t other : ids) {
      candidates.push_back({l2_squared(q, row(other), dim), other});
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Neighbor& a, const Neighbor& b) {
                if (a.dist != b.dist) return a.dist < b.dist;
                return a.id < b.id;
              });
    return selectNeighbors(id, candidates, limit);
  }

  void addLink(uint32_t src, uint32_t dst, int level) {
    auto& list = nodes[src].links[static_cast<size_t>(level)];
    if (std::find(list.begin(), list.end(), dst) != list.end()) return;
    list.push_back(dst);
    const size_t limit = maxConnections(level);
    if (list.size() <= limit) return;
    list = sortedNeighbors(src, list, limit);
  }

  void insert(uint32_t id) {
    const int level = randomLevel();
    const float* q = row(id);

    nodes.push_back(Node{});
    nodes.back().level = level;
    nodes.back().links.resize(static_cast<size_t>(level) + 1);
    for (int l = 0; l <= level; ++l) {
      nodes.back().links[static_cast<size_t>(l)].reserve(maxConnections(l));
    }

    if (entry < 0) {
      entry = static_cast<int>(id);
      top_level = level;
      return;
    }

    uint32_t ep = static_cast<uint32_t>(entry);
    for (int l = top_level; l > level; --l) {
      ep = greedySearch(q, ep, l);
    }

    const int first_level = std::min(level, top_level);
    for (int l = first_level; l >= 0; --l) {
      auto candidates =
          searchLayer(q, ep, static_cast<size_t>(ef_construction), l,
                      build_scratch);
      const size_t limit = maxConnections(l);
      auto selected = selectNeighbors(id, candidates, limit);
      nodes[id].links[static_cast<size_t>(l)] = selected;
      for (uint32_t other : selected) {
        addLink(other, id, l);
      }
      if (!candidates.empty()) ep = candidates.front().id;
    }

    if (level > top_level) {
      top_level = level;
      entry = static_cast<int>(id);
    }
  }
};

AnnIndex::AnnIndex(int dim, int M, int ef_construction, uint64_t seed) {
  if (dim <= 0) throw std::invalid_argument("AnnIndex: dim must be > 0");
  if (M < 2) throw std::invalid_argument("AnnIndex: M must be >= 2");
  if (ef_construction < 1) {
    throw std::invalid_argument("AnnIndex: ef_construction must be >= 1");
  }
  impl_ = std::make_unique<Impl>(dim, M, ef_construction, seed);
}

AnnIndex::~AnnIndex() = default;
AnnIndex::AnnIndex(AnnIndex&&) noexcept = default;
AnnIndex& AnnIndex::operator=(AnnIndex&&) noexcept = default;

void AnnIndex::build(const std::vector<std::vector<float>>& data) {
  Impl& s = *impl_;
  for (const auto& row : data) {
    if (static_cast<int>(row.size()) != s.dim) {
      throw std::invalid_argument("AnnIndex::build: row has wrong dimension");
    }
  }

  s.data.clear();
  s.data.resize(data.size() * static_cast<size_t>(s.dim));
  for (size_t i = 0; i < data.size(); ++i) {
    std::memcpy(s.data.data() + i * static_cast<size_t>(s.dim),
                data[i].data(), static_cast<size_t>(s.dim) * sizeof(float));
  }

  s.nodes.clear();
  s.nodes.reserve(data.size());
  s.entry = -1;
  s.top_level = -1;
  s.rng_state = s.seed;
  s.build_scratch.visited.assign(data.size(), 0);
  s.build_scratch.epoch = 0;

  for (size_t i = 0; i < data.size(); ++i) {
    s.insert(static_cast<uint32_t>(i));
  }
}

std::vector<uint32_t> AnnIndex::search(const float* query, size_t k,
                                       size_t ef_search) const {
  const Impl& s = *impl_;
  std::vector<uint32_t> result;
  if (query == nullptr || k == 0 || s.nodes.empty()) return result;
  k = std::min(k, s.nodes.size());
  if (ef_search < k) ef_search = k;

  uint32_t ep = static_cast<uint32_t>(s.entry);
  for (int l = s.top_level; l > 0; --l) {
    ep = s.greedySearch(query, ep, l);
  }

  Scratch scratch;
  scratch.visited.assign(s.nodes.size(), 0);
  auto found = s.searchLayer(query, ep, ef_search, 0, scratch);
  const size_t take = std::min(k, found.size());
  result.reserve(take);
  for (size_t i = 0; i < take; ++i) result.push_back(found[i].id);
  return result;
}

size_t AnnIndex::size() const { return impl_->nodes.size(); }
int AnnIndex::dim() const { return impl_->dim; }
int AnnIndex::max_level() const { return impl_->top_level; }
int AnnIndex::entry_point() const { return impl_->entry; }

size_t AnnIndex::memoryBytes() const {
  const Impl& s = *impl_;
  size_t bytes = s.data.capacity() * sizeof(float);
  bytes += s.nodes.capacity() * sizeof(Node);
  for (const Node& node : s.nodes) {
    bytes += node.links.capacity() * sizeof(std::vector<uint32_t>);
    for (const auto& list : node.links) {
      bytes += list.capacity() * sizeof(uint32_t);
    }
  }
  bytes += s.build_scratch.visited.capacity() * sizeof(uint32_t);
  return bytes;
}

}  // namespace mvdb::ann
