#include "ann_index.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

#ifndef ANN_M
#define ANN_M 16
#endif
#ifndef ANN_EF_CONSTRUCTION
#define ANN_EF_CONSTRUCTION 200
#endif
#ifndef ANN_NUM_QUERIES
#define ANN_NUM_QUERIES 500
#endif

constexpr int kDim = 128;
constexpr int kNumVectors = 10000;
constexpr int kNumQueries = ANN_NUM_QUERIES;
constexpr int kK = 10;
constexpr int kM = ANN_M;
constexpr int kEfConstruction = ANN_EF_CONSTRUCTION;
constexpr size_t kEfSearch[] = {16, 32, 64, 128, 256};

struct Rng {
  uint64_t state;

  explicit Rng(uint64_t seed) : state(seed) {}

  uint64_t next() {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Uniform in [lo, hi), with 24 random mantissa bits.
  float uniform(float lo, float hi) {
    const float u = static_cast<float>((next() >> 40) * (1.0 / 16777216.0));
    return lo + (hi - lo) * u;
  }
};

struct DistId {
  float dist;
  uint32_t id;
};

void fillUniform(std::vector<std::vector<float>>& data,
                 std::vector<std::vector<float>>& queries, uint64_t seed) {
  Rng data_rng(seed);
  Rng query_rng(seed ^ 0x9e3779b97f4a7c15ULL);
  data.assign(kNumVectors, std::vector<float>(kDim));
  queries.assign(kNumQueries, std::vector<float>(kDim));
  for (auto& row : data) {
    for (float& value : row) value = data_rng.uniform(-1.0f, 1.0f);
  }
  for (auto& row : queries) {
    for (float& value : row) value = query_rng.uniform(-1.0f, 1.0f);
  }
}

void fillClustered(std::vector<std::vector<float>>& data,
                   std::vector<std::vector<float>>& queries, uint64_t seed) {
  constexpr int kClusters = 20;
  Rng rng(seed);
  std::vector<std::vector<float>> centers(
      kClusters, std::vector<float>(kDim));
  for (auto& center : centers) {
    for (float& value : center) value = rng.uniform(-3.0f, 3.0f);
  }
  data.assign(kNumVectors, std::vector<float>(kDim));
  queries.assign(kNumQueries, std::vector<float>(kDim));
  auto sample = [&](std::vector<float>& row) {
    const auto& center = centers[static_cast<size_t>(rng.next() % kClusters)];
    for (int d = 0; d < kDim; ++d) {
      row[static_cast<size_t>(d)] = center[static_cast<size_t>(d)] +
                                    rng.uniform(-0.35f, 0.35f);
    }
  };
  for (auto& row : data) sample(row);
  for (auto& row : queries) sample(row);
}

std::vector<uint32_t> exactTopK(
    const std::vector<std::vector<float>>& data,
    const std::vector<float>& query, std::vector<DistId>& scratch) {
  scratch.resize(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    scratch[i] = {mvdb::ann::l2_squared(data[i].data(), query.data(), kDim),
                  static_cast<uint32_t>(i)};
  }
  const size_t k = std::min<size_t>(kK, data.size());
  std::partial_sort(
      scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(k),
      scratch.end(), [](const DistId& a, const DistId& b) {
        if (a.dist != b.dist) return a.dist < b.dist;
        return a.id < b.id;
      });
  std::vector<uint32_t> out;
  out.reserve(k);
  for (size_t i = 0; i < k; ++i) out.push_back(scratch[i].id);
  return out;
}

bool validResult(const std::vector<uint32_t>& ids, size_t n) {
  if (ids.size() != std::min<size_t>(kK, n)) return false;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] >= n) return false;
    for (size_t j = 0; j < i; ++j) {
      if (ids[i] == ids[j]) return false;
    }
  }
  return true;
}

size_t overlap(const std::vector<uint32_t>& found,
               const std::vector<uint32_t>& truth) {
  size_t count = 0;
  for (uint32_t id : found) {
    if (std::find(truth.begin(), truth.end(), id) != truth.end()) ++count;
  }
  return count;
}

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  const size_t index = std::min(
      sorted.size() - 1,
      static_cast<size_t>(p * static_cast<double>(sorted.size())));
  return sorted[index];
}

struct DatasetStats {
  std::string name;
  uint64_t seed = 0;
  double build_ms = 0.0;
  double memory_mib = 0.0;
  int max_level = 0;
  std::vector<double> recall;
  std::vector<double> p50_us;
  std::vector<double> p99_us;
  std::vector<double> mean_us;
};

DatasetStats runDataset(const std::string& name, uint64_t seed,
                        bool clustered) {
  std::vector<std::vector<float>> data;
  std::vector<std::vector<float>> queries;
  if (clustered) {
    fillClustered(data, queries, seed);
  } else {
    fillUniform(data, queries, seed);
  }

  std::vector<std::vector<uint32_t>> truth;
  truth.reserve(queries.size());
  std::vector<DistId> scratch;
  scratch.reserve(data.size());
  for (const auto& query : queries) {
    truth.push_back(exactTopK(data, query, scratch));
  }

  DatasetStats stats;
  stats.name = name;
  stats.seed = seed;
  stats.recall.assign(std::size(kEfSearch), 0.0);
  stats.p50_us.assign(std::size(kEfSearch), 0.0);
  stats.p99_us.assign(std::size(kEfSearch), 0.0);
  stats.mean_us.assign(std::size(kEfSearch), 0.0);

  mvdb::ann::AnnIndex index(kDim, kM, kEfConstruction, seed);
  const auto build_start = std::chrono::steady_clock::now();
  index.build(data);
  const auto build_end = std::chrono::steady_clock::now();
  stats.build_ms =
      std::chrono::duration<double, std::milli>(build_end - build_start).count();
  stats.memory_mib =
      static_cast<double>(index.memoryBytes()) / (1024.0 * 1024.0);
  stats.max_level = index.max_level();

  std::vector<double> latencies;
  latencies.reserve(queries.size());
  for (size_t e = 0; e < std::size(kEfSearch); ++e) {
    const size_t ef = kEfSearch[e];

    // Warm the instruction/data caches without including this in the
    // measured percentiles.
    for (size_t q = 0; q < std::min<size_t>(16, queries.size()); ++q) {
      (void)index.search(queries[q].data(), kK, ef);
    }

    size_t hits = 0;
    latencies.clear();
    for (size_t q = 0; q < queries.size(); ++q) {
      const auto start = std::chrono::steady_clock::now();
      auto ids = index.search(queries[q].data(), kK, ef);
      const auto end = std::chrono::steady_clock::now();
      latencies.push_back(
          std::chrono::duration<double, std::micro>(end - start).count());
      if (!validResult(ids, data.size())) {
        std::cerr << "FATAL: invalid result vector (dataset=" << name
                  << ", query=" << q << ", ef=" << ef << ")\n";
        std::exit(2);
      }
      hits += overlap(ids, truth[q]);
    }
    stats.recall[e] =
        static_cast<double>(hits) /
        (static_cast<double>(queries.size()) * static_cast<double>(kK));

    std::sort(latencies.begin(), latencies.end());
    double sum = 0.0;
    for (double value : latencies) sum += value;
    stats.mean_us[e] = sum / static_cast<double>(latencies.size());
    stats.p50_us[e] = percentile(latencies, 0.50);
    stats.p99_us[e] = percentile(latencies, 0.99);
  }
  return stats;
}

}  // namespace

int main() {
  std::cout << "HNSW ANN prototype benchmark\n";
  std::cout << "vectors=" << kNumVectors << " dim=" << kDim
            << " queries=" << kNumQueries << " k=" << kK << " M=" << kM
            << " ef_construction=" << kEfConstruction << "\n";
  std::cout << "metric=squared L2, distance=AVX2+FMA, build=deterministic "
               "SplitMix64 levels\n\n";

  const std::vector<DatasetStats> all = {
      runDataset("uniform", 42, false),
      runDataset("uniform", 43, false),
      runDataset("uniform", 44, false),
      runDataset("clustered", 42, true),
  };

  std::cout << std::left << std::setw(11) << "dataset" << std::right
            << std::setw(5) << "seed" << std::setw(11) << "build_ms"
            << std::setw(10) << "mem_MiB" << std::setw(6) << "level"
            << " |" << std::setw(5) << "ef" << std::setw(11) << "recall@10"
            << std::setw(10) << "p50_us" << std::setw(10) << "p99_us"
            << std::setw(11) << "mean_us" << "\n";
  std::cout << std::string(93, '-') << "\n";
  std::cout << std::fixed << std::setprecision(4);
  for (const DatasetStats& stats : all) {
    for (size_t e = 0; e < std::size(kEfSearch); ++e) {
      std::cout << std::left << std::setw(11) << stats.name << std::right
                << std::setw(5) << stats.seed << std::setw(11) << std::setprecision(1)
                << stats.build_ms << std::setw(10) << std::setprecision(2)
                << stats.memory_mib << std::setw(6) << stats.max_level << " |"
                << std::setw(5) << kEfSearch[e] << std::setw(11)
                << std::setprecision(4) << stats.recall[e] << std::setw(10)
                << std::setprecision(2) << stats.p50_us[e] << std::setw(10)
                << stats.p99_us[e] << std::setw(11) << stats.mean_us[e] << "\n";
    }
  }

  std::cout << "\nSummary\n";
  std::cout << std::setprecision(4);
  bool all_pass = true;
  for (const DatasetStats& stats : all) {
    size_t best_ef = 0;
    bool found = false;
    for (size_t e = 0; e < std::size(kEfSearch); ++e) {
      if (stats.recall[e] >= 0.98) {
        best_ef = e;
        found = true;
        break;
      }
    }
    if (found) {
      std::cout << "  " << stats.name << "/" << stats.seed
                << ": recall>=0.98 at ef=" << kEfSearch[best_ef] << " ("
                << stats.recall[best_ef] << ", p50=" << std::setprecision(2)
                << stats.p50_us[best_ef] << " us, p99=" << stats.p99_us[best_ef]
                << " us)" << std::setprecision(4) << "\n";
    } else {
      all_pass = false;
      std::cout << "  " << stats.name << "/" << stats.seed
                << ": recall<0.98 even at ef=" << kEfSearch[std::size(kEfSearch) - 1]
                << " (max="
                << stats.recall[std::size(kEfSearch) - 1] << ")\n";
    }
  }
  std::cout << "  overall: " << (all_pass ? "PASS" : "FAIL")
            << " (every dataset must reach recall>=0.98)\n";
  std::cout << "  note: timings are machine-dependent; recall/build are "
               "deterministic for a fixed seed\n";
  return all_pass ? 0 : 1;
}
