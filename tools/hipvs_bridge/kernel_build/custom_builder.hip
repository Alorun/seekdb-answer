/* Custom GPU construction only. Fixed hipVS owns search and serialized indexes.
 * Adapted construction helpers retain their upstream notices under vendor/.
 */
#include <hip/hip_runtime.h>
#include <cassert>
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <cstring>
#include <type_traits>
#include <cmath>
#include <hip/hip_fp16.h>
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/brute_force.h>
#include <cuvs/neighbors/nn_descent.h>
#include "custom_builder.h"
#include "vendor/cpp/src/neighbors/detail/nn_descent.cuh"
#include "vendor/cpp/src/neighbors/detail/cagra/graph_core.cuh"

namespace {
using Clock = std::chrono::steady_clock;
void check(cuvsError_t status, const char* operation) {
  if (status != CUVS_SUCCESS) {
    const char* reason = cuvsGetLastErrorText();
    throw std::runtime_error(std::string(operation) + ": " + (reason ? reason : "cuVS error"));
  }
}
struct Params {
  cuvsNNDescentIndexParams_t p = nullptr;
  Params() { check(cuvsNNDescentIndexParamsCreate(&p), "NNDescentIndexParamsCreate"); }
  ~Params() { if (p) cuvsNNDescentIndexParamsDestroy(p); }
};
struct Index {
  cuvsNNDescentIndex_t p = nullptr;
  Index() {
    check(cuvsNNDescentIndexCreate(&p), "NNDescentIndexCreate");
    p->dtype = DLDataType{static_cast<uint8_t>(kDLUInt), 32, 1};
  }
  ~Index() { if (p) cuvsNNDescentIndexDestroy(p); }
};
DLManagedTensor tensor(void* data, int64_t* shape, uint8_t code) {
  DLManagedTensor result{};
  result.dl_tensor.data = data;
  result.dl_tensor.device = DLDevice{kDLCPU, 0};
  result.dl_tensor.ndim = 2;
  result.dl_tensor.dtype = DLDataType{code, 32, 1};
  result.dl_tensor.shape = shape;
  return result;
}
DLManagedTensor device_tensor(void* data, int64_t* shape, uint8_t code, uint8_t bits) {
  DLManagedTensor result{};
  result.dl_tensor.data = data;
  result.dl_tensor.device = DLDevice{kDLCUDA, 0};
  result.dl_tensor.ndim = 2;
  result.dl_tensor.dtype = DLDataType{code, bits, 1};
  result.dl_tensor.shape = shape;
  result.dl_tensor.strides = nullptr;
  result.dl_tensor.byte_offset = 0;
  return result;
}
double seconds(Clock::time_point start, Clock::time_point end) {
  return std::chrono::duration<double>(end - start).count();
}
// Dev-only timing sink: duplicates the phase line into $SK_CUVS_TIMING_FILE so
// it survives the observer's stderr redirection. No-op unless set.
void log_line(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  fflush(stderr);
  const char* path = std::getenv("SK_CUVS_TIMING_FILE");
  if (path == nullptr || path[0] == '\0') return;
  FILE* file = std::fopen(path, "a");
  if (file == nullptr) return;
  va_start(args, format);
  vfprintf(file, format, args);
  va_end(args);
  std::fclose(file);
}
// Graph-build tuning knobs. Defaults are the shipped values; the environment
// overrides exist for offline sweeps (build cost vs Recall) on a GPU host and
// are ignored unless set to a valid integer in range.
int env_int(const char* name, int fallback, int low, int high) {
  const char* text = std::getenv(name);
  if (text == nullptr || text[0] == '\0') return fallback;
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < low || value > high) {
    fprintf(stderr, "SEEKDB_GPU_BUILD_TUNE_IGNORED name=%s value=%s\n", name, text);
    return fallback;
  }
  return static_cast<int>(value);
}
struct Tuning {
  int degree;
  int internal_degree;
  int iterations;
  int final_degree;
  Tuning()
      : degree(env_int("SK_CUVS_NND_DEGREE", 256, 32, 1024)),
        internal_degree(env_int("SK_CUVS_NND_INTERNAL_DEGREE", 384, 32, 1024)),
        iterations(env_int("SK_CUVS_NND_ITER", 50, 1, 400)),
        final_degree(env_int("SK_CUVS_FINAL_DEGREE", 128, 16, 1024)) {
    if (internal_degree < degree) internal_degree = degree;
    if (final_degree > degree) final_degree = degree;
  }
};
const Tuning& tuning() {
  static const Tuning value;
  return value;
}
// Exact GPU kNN graph builder.  For 250K rows the approximate graph builders
// that ship with the library are dominated by their search/refinement cost,
// while an exact all-pairs top-k is a dense problem that maps well onto the
// GPU.  The frozen hipVS brute-force path is used as the candidate generator
// (GPU GEMM + device-side k-selection); the CAGRA graph optimizer then prunes
// those exact candidates down to the requested final degree and adds the
// reverse edges, and the fixed library search implementation serves queries.
// Default algorithm for the contest-sized offline DDL path (n <= 400K): the
// exact-kNN build is cheaper than 50 NN-descent iterations there.  Larger
// datasets keep the library NN-descent path because all-pairs cost grows with
// n^2.  SK_CUVS_BUILD_ALGO=nnd forces the old path, the brute names force this
// one.
bool brute_algo(long n) {
  const char* algo = std::getenv("SK_CUVS_BUILD_ALGO");
  if (algo != nullptr && algo[0] != '\0') {
    if (std::strcmp(algo, "nnd") == 0) return false;
    return std::strcmp(algo, "brute") == 0 || std::strcmp(algo, "bruteforce") == 0 ||
           std::strcmp(algo, "bf") == 0;
  }
  return n <= 400000;
}
// Diagnostic-only graph: a ring over the dataset, no distance computation.  It
// exists to measure the DDL/snapshot/persistence floor of the timed build with
// an almost-free GPU stage; Recall is meaningless with it.
bool floor_algo() {
  const char* algo = std::getenv("SK_CUVS_BUILD_ALGO");
  return algo != nullptr && std::strcmp(algo, "floor") == 0;
}
struct BruteForceIndex {
  cuvsBruteForceIndex_t p = nullptr;
  BruteForceIndex() { check(cuvsBruteForceIndexCreate(&p), "BruteForceIndexCreate"); }
  ~BruteForceIndex() {
    if (p) cuvsBruteForceIndexDestroy(p);
  }
};
// FP32 -> FP16 conversion for the candidate GEMM.  The distance matrix and
// norms stay FP32, so this only changes the precision of the multiply inputs.
__global__ void float_to_half_kernel(const float* input, half* output, size_t count, float scale) {
  const size_t index = static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < count) output[index] = __float2half_rn(input[index] * scale);
}
// Typed implementation: the candidate GEMM runs in ElementType while the
// pairwise distances and norms stay FP32 (DistanceT defaults to float), so a
// half candidate search is safe even for SIFT (values 0..255) and the exact
// re-sort in sort_knn_graph still runs on the original FP32 dataset.
template <typename ElementType>
void build_knn_bruteforce_typed(cuvsResources_t resources, const float* base, long n, long dim,
                                int degree, uint8_t bits,
                                raft::host_matrix<uint32_t, int64_t>& knn) {
  auto& res = *reinterpret_cast<raft::resources*>(resources);
  auto stream = raft::resource::get_cuda_stream(res);
  const auto start = Clock::now();
  const long chunk = std::min<long>(n, env_int("SK_CUVS_BF_CHUNK", 8192, 1024, 262144));
  // Ask for one extra neighbour so that dropping the self edge still leaves a
  // full `degree` of distinct candidates.  k must stay <= 256: above that the
  // frozen library falls back from warp-sort to a multi-pass radix selection.
  const int request = degree < 256 ? degree + 1 : degree;
  auto d_base = raft::make_device_matrix<ElementType, int64_t>(res, n, dim);
  const size_t element_count = static_cast<size_t>(n) * dim;
  if constexpr (std::is_same_v<ElementType, float>) {
    raft::copy(d_base.data_handle(), base, element_count, stream);
  } else {
    // Power-of-two rescale keeps every multiply input well inside the FP16
    // range even when the hidden dataset is not normalized; L2 ranking is
    // invariant under a positive uniform scale and the exact re-sort uses FP32.
    float max_abs = 0.0f;
    #pragma omp parallel for reduction(max : max_abs)
    for (long i = 0; i < n * dim; ++i) max_abs = std::max(max_abs, std::fabs(base[i]));
    const float limit = std::sqrt(30000.0f / static_cast<float>(dim));
    float scale = 1.0f;
    if (max_abs > limit && std::isfinite(max_abs))
      scale = std::exp2(std::floor(std::log2(limit / max_abs)));
    auto d_float = raft::make_device_matrix<float, int64_t>(res, n, dim);
    raft::copy(d_float.data_handle(), base, element_count, stream);
    const int threads = 256;
    const int blocks = static_cast<int>((element_count + threads - 1) / threads);
    float_to_half_kernel<<<blocks, threads, 0, stream>>>(
        d_float.data_handle(), d_base.data_handle(), element_count, scale);
  }
  check(cuvsStreamSync(resources), "BruteForce dataset upload sync");
  const auto uploaded = Clock::now();
  BruteForceIndex index;
  {
    int64_t dataset_shape[] = {n, dim};
    DLManagedTensor dataset = device_tensor(d_base.data_handle(), dataset_shape, kDLFloat, bits);
    check(cuvsBruteForceBuild(resources, &dataset, L2Expanded, 0.0f, index.p), "BruteForceBuild");
  }
  const auto built = Clock::now();
  std::vector<int64_t> staging(static_cast<size_t>(std::min<long>(chunk, n)) * request);
  for (long row_start = 0; row_start < n; row_start += chunk) {
    const long rows = std::min<long>(chunk, n - row_start);
    auto d_neighbors = raft::make_device_matrix<int64_t, int64_t>(res, rows, request);
    auto d_distances = raft::make_device_matrix<float, int64_t>(res, rows, request);
    int64_t query_shape[] = {rows, dim};
    int64_t output_shape[] = {rows, request};
    DLManagedTensor queries = device_tensor(
        d_base.data_handle() + static_cast<size_t>(row_start) * dim, query_shape, kDLFloat, bits);
    DLManagedTensor neighbors = device_tensor(d_neighbors.data_handle(), output_shape, kDLInt, 64);
    DLManagedTensor distances =
        device_tensor(d_distances.data_handle(), output_shape, kDLFloat, 32);
    cuvsFilter filter;
    filter.type = NO_FILTER;
    filter.addr = static_cast<uintptr_t>(0);
    check(cuvsBruteForceSearch(resources, index.p, &queries, &neighbors, &distances, filter),
          "BruteForceSearch");
    check(cuvsStreamSync(resources), "BruteForceSearch sync");
    const size_t count = static_cast<size_t>(rows) * request;
    raft::copy(staging.data(), d_neighbors.data_handle(), count, stream);
    check(cuvsStreamSync(resources), "BruteForce copy sync");
    for (long r = 0; r < rows; ++r) {
      const long row = row_start + r;
      const int64_t* candidates = staging.data() + static_cast<size_t>(r) * request;
      int column = 0;
      for (int j = 0; j < request && column < degree; ++j) {
        const int64_t candidate = candidates[j];
        if (candidate == row) continue;
        if (candidate < 0 || candidate >= n)
          throw std::runtime_error("brute-force neighbour out of range");
        knn(row, column++) = static_cast<uint32_t>(candidate);
      }
      if (column == 0) throw std::runtime_error("brute-force graph has no neighbour");
      while (column < degree) knn(row, column++) = knn(row, 0);
    }
  }
  const auto end = Clock::now();
  log_line("SEEKDB_BF_BUILD n=%ld dim=%ld degree=%d k=%d bits=%d chunk=%ld upload_sec=%.6f "
           "build_sec=%.6f search_sec=%.6f sec=%.6f\n",
           n, dim, degree, request, static_cast<int>(bits), chunk, seconds(start, uploaded),
           seconds(uploaded, built), seconds(built, end), seconds(start, end));
}

// Returns a host knn graph of `degree` columns per row.  The brute-force
// search includes each row itself as its nearest neighbour, so the self edge
// is replaced by duplicating the nearest real neighbour; the graph optimizer
// deduplicates it.  Queries are streamed in chunks: the pairwise distance tile
// of the frozen brute-force path is the largest allocation, so keeping the
// per-query output small keeps the peak device footprint below the frozen
// NN-descent baseline.  The half candidate search falls back to FP32 if the
// frozen library rejects it.
void build_knn_bruteforce(cuvsResources_t resources, const float* base, long n, long dim,
                          int degree, raft::host_matrix<uint32_t, int64_t>& knn) {
  const auto start = Clock::now();
  if (env_int("SK_CUVS_BF_FP16", 1, 0, 1) != 0) {
    try {
      build_knn_bruteforce_typed<half>(resources, base, n, dim, degree, 16, knn);
      log_line("SEEKDB_BF_DONE n=%ld dim=%ld degree=%d fp16=1 sec=%.6f\n", n, dim, degree,
               seconds(start, Clock::now()));
      return;
    } catch (const std::exception& error) {
      log_line("SEEKDB_BF_FP16_FALLBACK n=%ld dim=%ld degree=%d: %s\n", n, dim, degree,
               error.what());
    }
  }
  build_knn_bruteforce_typed<float>(resources, base, n, dim, degree, 32, knn);
  log_line("SEEKDB_BF_DONE n=%ld dim=%ld degree=%d fp16=0 sec=%.6f\n", n, dim, degree,
           seconds(start, Clock::now()));
}
}

extern "C" int seekdb_cuvs_custom_build(cuvsResources_t resources, const float* base,
                                      long n, long dim, cuvsCagraIndex_t output,
                                      unsigned flags) {
  try {
    if (!base || !output || n < 1024 || n >= std::numeric_limits<int>::max() - 1 ||
        dim <= 0 || dim > std::numeric_limits<int>::max() || flags > 7)
      throw std::invalid_argument("invalid custom GPU build arguments");
    auto& res = *reinterpret_cast<raft::resources*>(resources);
    if (raft::host_warp_size(raft::resource::get_cuda_stream(res)) != 32)
      throw std::runtime_error("custom builder currently requires wave32");
    const Tuning& tune = tuning();
    const auto start = Clock::now();
    const bool use_gnnd = (flags & 2) != 0;
    const bool use_brute = !use_gnnd && brute_algo(n);
    // The exact-kNN path defaults to a 128-degree candidate list, which is the
    // smallest list that makes the optimizer take its exact-degree fast path
    // (no detour prune) while keeping the graph quality of the 256 list.
    const int knn_degree = use_brute ? env_int("SK_CUVS_BF_DEGREE", 128, 32, 256) : tune.degree;
    const int final_degree = std::min(tune.final_degree, knn_degree);
    auto knn = raft::make_host_matrix<uint32_t, int64_t>(n, knn_degree);
    int64_t ds[] = {n, dim}, gs[] = {n, knn_degree};
    auto dataset = tensor(const_cast<float*>(base), ds, kDLFloat);
    auto graph = tensor(knn.data_handle(), gs, kDLUInt);
    if (use_gnnd) {
      namespace own = seekdb_cagra_build::nnd;
      // Same get_build_config arithmetic as the frozen NN-descent path.
      const size_t extended = own::roundUp64(static_cast<size_t>(tune.degree * 1.3));
      const size_t extended_internal =
          own::roundUp64(static_cast<size_t>(tune.internal_degree * 1.3));
      auto buffer = raft::make_host_matrix<int, int64_t>(n, own::align64::roundUp(extended));
      own::BuildConfig config{};
      config.max_dataset_size = n;
      config.dataset_dim = dim;
      config.node_degree = extended;
      config.internal_node_degree = extended_internal;
      config.max_iterations = tune.iterations;
      config.termination_threshold = 0.0001f;
      config.output_graph_degree = tune.degree;
      config.metric = cuvs::distance::DistanceType::L2Expanded;
      config.reduce_join_sync = true;
      own::GNND<const float, int> builder(res, config);
      builder.build(base, static_cast<int>(n), buffer.data_handle(), false, nullptr);
      #pragma omp parallel for
      for (long row = 0; row < n; ++row)
        for (int col = 0; col < tune.degree; ++col)
          knn(row, col) = buffer.data_handle()[size_t(row) * extended + col];
    } else if (use_brute) {
      build_knn_bruteforce(resources, base, n, dim, knn_degree, knn);
    } else if (floor_algo()) {
      #pragma omp parallel for
      for (long row = 0; row < n; ++row)
        for (int col = 0; col < knn_degree; ++col)
          knn(row, col) = static_cast<uint32_t>((row + 1 + col) % n);
    } else {
      Params params;
      Index index;
      params.p->metric = L2Expanded;
      params.p->graph_degree = tune.degree;
      params.p->intermediate_graph_degree = tune.internal_degree;
      params.p->max_iterations = tune.iterations;
      params.p->termination_threshold = 0.0001f;
      params.p->return_distances = false;
      check(cuvsNNDescentBuild(resources, params.p, &dataset, &graph, index.p), "NNDescentBuild");
      check(cuvsStreamSync(resources), "NNDescent sync");
    }
    raft::resource::sync_stream(res);
    const auto joined = Clock::now();
    auto data_view = raft::make_host_matrix_view<const float, int64_t>(base, n, dim);
    seekdb_cagra_build::graph::sort_knn_graph(res, cuvs::distance::DistanceType::L2Expanded,
                                             data_view, knn.view());
    raft::resource::sync_stream(res);
    const auto sorted = Clock::now();
    auto final_graph = raft::make_host_matrix<uint32_t, int64_t>(n, final_degree);
    seekdb_cagra_build::graph::optimize(res, knn.view(), final_graph.view(),
                                      false, true, flags & 1, flags & 4);
    raft::resource::sync_stream(res);
    const auto pruned = Clock::now();
    int64_t final_shape[] = {n, final_degree};
    auto final_tensor = tensor(final_graph.data_handle(), final_shape, kDLUInt);
    check(cuvsCagraIndexFromArgs(resources, L2Expanded, &final_tensor, &dataset, output),
          "CagraIndexFromArgs");
    check(cuvsStreamSync(resources), "CagraIndexFromArgs sync");
    const auto end = Clock::now();
    log_line("SEEKDB_GPU_BUILD flags=%u n=%ld dim=%ld niter=%d degree=%d "
             "internal_degree=%d final_degree=%d "
             "nnd_sec=%.6f sort_sec=%.6f prune_reverse_sec=%.6f attach_sec=%.6f total_sec=%.6f\n",
             flags, n, dim, tune.iterations, knn_degree, tune.internal_degree,
             final_degree, seconds(start, joined), seconds(joined, sorted),
             seconds(sorted, pruned), seconds(pruned, end), seconds(start, end));
    return 0;
  } catch (const std::exception& error) {
    log_line("SEEKDB_GPU_BUILD_FAILED flags=%u: %s\n", flags, error.what());
    return 1;
  } catch (...) {
    log_line("SEEKDB_GPU_BUILD_FAILED flags=%u: unknown exception\n", flags);
    return 1;
  }
}
