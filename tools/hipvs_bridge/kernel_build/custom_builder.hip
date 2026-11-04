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
#include <cuvs/core/c_api.h>
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
        iterations(env_int("SK_CUVS_NND_ITER", 40, 1, 400)),
        final_degree(env_int("SK_CUVS_FINAL_DEGREE", 128, 16, 1024)) {
    if (internal_degree < degree) internal_degree = degree;
    if (final_degree > degree) final_degree = degree;
  }
};
const Tuning& tuning() {
  static const Tuning value;
  return value;
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
    auto knn = raft::make_host_matrix<uint32_t, int64_t>(n, tune.degree);
    int64_t ds[] = {n, dim}, gs[] = {n, tune.degree};
    auto dataset = tensor(const_cast<float*>(base), ds, kDLFloat);
    auto graph = tensor(knn.data_handle(), gs, kDLUInt);
    if (flags & 2) {
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
    auto final_graph = raft::make_host_matrix<uint32_t, int64_t>(n, tune.final_degree);
    seekdb_cagra_build::graph::optimize(res, knn.view(), final_graph.view(),
                                      false, true, flags & 1, flags & 4);
    raft::resource::sync_stream(res);
    const auto pruned = Clock::now();
    int64_t final_shape[] = {n, tune.final_degree};
    auto final_tensor = tensor(final_graph.data_handle(), final_shape, kDLUInt);
    check(cuvsCagraIndexFromArgs(resources, L2Expanded, &final_tensor, &dataset, output),
          "CagraIndexFromArgs");
    check(cuvsStreamSync(resources), "CagraIndexFromArgs sync");
    const auto end = Clock::now();
    log_line("SEEKDB_GPU_BUILD flags=%u n=%ld dim=%ld niter=%d degree=%d "
             "internal_degree=%d final_degree=%d "
             "nnd_sec=%.6f sort_sec=%.6f prune_reverse_sec=%.6f attach_sec=%.6f total_sec=%.6f\n",
             flags, n, dim, tune.iterations, tune.degree, tune.internal_degree,
             tune.final_degree, seconds(start, joined), seconds(joined, sorted),
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
