#include "executor.h"

#include "ann/ann_q7.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <limits>
#include <immintrin.h>
#include <queue>
#include <set>
#include <thread>
#include <sched.h>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace mvdb {

namespace {

// Number of CPUs this process may actually run on (honours taskset/cgroup).
int usableCpuCount() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (sched_getaffinity(0, sizeof(set), &set) != 0) {
    return static_cast<int>(std::thread::hardware_concurrency());
  }
  int n = CPU_COUNT(&set);
  return n > 0 ? n : static_cast<int>(std::thread::hardware_concurrency());
}

}  // namespace

ScanPool::ScanPool() {
  // A second physical core nearly halves the q7 scan; SMT siblings compete for
  // the same execution units.  Pick a non-sibling pair from the affine set, so
  // a container with a 2-CPU quota and a wide affinity mask still uses two
  // physical cores.  Only a genuine 2-CPU sibling pinning falls back to one.
  if (std::getenv("MVDB_NO_2T") != nullptr) {
    usable_ = false;
    return;
  }
  const bool forced = std::getenv("MVDB_TWO_THREADS") != nullptr;
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof(set), &set) != 0) {
    usable_ = forced;
    return;
  }
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE && cpus.size() < 16; ++c) {
    if (CPU_ISSET(c, &set)) cpus.push_back(c);
  }
  if (cpus.size() < 2) {
    usable_ = false;
    return;
  }
  auto siblings = [](int cpu) {
    std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu) +
                     "/topology/thread_siblings_list");
    std::string line;
    std::getline(in, line);
    return line;
  };
  auto has_cpu = [](const std::string& list, int cpu) {
    size_t p = 0;
    while (p < list.size()) {
      size_t q = list.find_first_of(",-", p);
      std::string tok = list.substr(p, q == std::string::npos ? q : q - p);
      try {
        if (std::stoi(tok) == cpu) return true;
      } catch (...) {
      }
      if (q == std::string::npos) break;
      if (list[q] == '-') {
        size_t r = list.find(',', q + 1);
        try {
          int lo = std::stoi(list.substr(p, q - p));
          int hi = std::stoi(list.substr(q + 1, r == std::string::npos
                                                       ? std::string::npos
                                                       : r - q - 1));
          if (cpu >= lo && cpu <= hi) return true;
        } catch (...) {
        }
        if (r == std::string::npos) break;
        p = r + 1;
        continue;
      }
      p = q + 1;
    }
    return false;
  };
  std::vector<std::string> lists;
  lists.reserve(cpus.size());
  for (int c : cpus) lists.push_back(siblings(c));
  bool found_pair = false;
  for (size_t i = 0; i < cpus.size() && !found_pair; ++i) {
    for (size_t j = i + 1; j < cpus.size(); ++j) {
      if (lists[i].empty() || lists[j].empty() ||
          !has_cpu(lists[i], cpus[j])) {
        found_pair = true;
        main_cpu_ = cpus[i];
        worker_cpu_ = cpus[j];
        break;
      }
    }
  }
  usable_ = forced || found_pair;
}


ScanPool::~ScanPool() {
  if (!worker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_.store(true, std::memory_order_relaxed);
  }
  cv_.notify_one();
  worker_.join();
}

void ScanPool::workerLoop() {
  uint64_t seen = 0;
  for (;;) {
    uint64_t spins = 0;
    while (seq_.load(std::memory_order_acquire) == seen) {
      if (stop_.load(std::memory_order_relaxed)) return;
      if (++spins > 40000) {
        // Idle for ~1ms: stop burning the core and wait for a wake-up.
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(2), [this, seen] {
          return stop_.load(std::memory_order_relaxed) ||
                 seq_.load(std::memory_order_acquire) != seen;
        });
        spins = 0;
      } else {
        _mm_pause();
      }
    }
    seen = seq_.load(std::memory_order_acquire);
    if (stop_.load(std::memory_order_relaxed)) return;
    try {
      job_();
    } catch (...) {
      error_ = std::current_exception();
    }
    done_.store(seen, std::memory_order_release);
  }
}

void ScanPool::run(const std::function<void()>& main_job,
                   const std::function<void()>& helper_job) {
  if (!usable_) {
    main_job();
    helper_job();
    return;
  }
  job_ = helper_job;
  uint64_t ticket = 0;
  {
    // Publish under the mutex so a worker that is about to sleep either sees
    // the new ticket in its wait predicate or is woken by the notify below.
    std::lock_guard<std::mutex> lk(mu_);
    if (!worker_.joinable()) {
      // Start the helper lazily: a request/response driver never pipelines, so
      // no idle spinner is created for the common case.
      worker_ = std::thread([this] { workerLoop(); });
    }
    ticket = seq_.load(std::memory_order_relaxed) + 1;
    seq_.store(ticket, std::memory_order_release);
  }
  cv_.notify_one();
  main_job();
  while (done_.load(std::memory_order_acquire) != ticket) _mm_pause();
  if (error_) {
    std::exception_ptr err = error_;
    error_ = nullptr;
    std::rethrow_exception(err);
  }
}

namespace {

struct OutCol {
  enum class Kind { Value, Distance } kind = Kind::Value;
  int col = -1;
  std::string header;
  std::vector<float> query;
};

struct KeyValue {
  enum class Kind { Int, Double, Str } kind = Kind::Int;
  int64_t i = 0;
  double d = 0;
  std::string str;
};

struct OrderSpec {
  bool is_distance = false;
  int col = -1;              // value column
  std::vector<float> query;  // distance query
  bool desc = false;
};

struct Candidate {
  int64_t pk = 0;
  std::vector<std::string> out;
  std::vector<KeyValue> keys;
};

std::string formatValue(const Value& v) {
  switch (v.type) {
    case Value::Type::Int:
      return std::to_string(v.i);
    case Value::Type::Str:
      return std::string(v.str());
    case Value::Type::Vec: {
      const std::span<const float> vals = v.vec();
      std::string out = "[";
      for (size_t i = 0; i < vals.size(); ++i) {
        if (i) out += ",";
        out += formatDouble(vals[i]);
      }
      out += "]";
      return out;
    }
  }
  return {};
}

std::vector<std::string> formatProjection(const Row& row,
                                         const std::vector<OutCol>& cols) {
  std::vector<std::string> out;
  out.reserve(cols.size());
  for (const auto& col : cols) {
    if (col.kind == OutCol::Kind::Value) {
      out.push_back(formatValue(row.values[col.col]));
    } else {
      out.push_back(formatDouble(l2Distance(row.values[col.col].vec(), col.query)));
    }
  }
  return out;
}

bool resolveConditions(const Table& table, const std::vector<Condition>& where,
                       std::vector<ResolvedCond>& out, Status& err) {
  for (const auto& c : where) {
    int idx = findColumn(table.schema, c.column);
    if (idx < 0) {
      err = Status::Error(Err::kColumnNotFound, "unknown column: " + c.column);
      return false;
    }
    const Column& col = table.schema.columns[idx];
    if (col.type == ColumnType::Vector) {
      err = Status::Error(Err::kUnsupported, "comparison on VECTOR column is not supported");
      return false;
    }
    if (col.type == ColumnType::BigInt && c.value.type != Value::Type::Int) {
      err = Status::Error(Err::kInvalidValue, "BIGINT column requires integer literal: " + c.column);
      return false;
    }
    if (col.type == ColumnType::Varchar && c.value.type != Value::Type::Str) {
      err = Status::Error(Err::kInvalidValue, "VARCHAR column requires string literal: " + c.column);
      return false;
    }
    ResolvedCond rc;
    rc.col = idx;
    rc.op = c.op;
    rc.literal = c.value;
    out.push_back(std::move(rc));
  }
  return true;
}

bool matchConditions(const Row& row, const std::vector<ResolvedCond>& conds,
                     Status& err) {
  for (const auto& c : conds) {
    const Value& v = row.values[c.col];
    int cmp = 0;
    if (v.type == Value::Type::Int) {
      if (c.literal.type != Value::Type::Int) {
        err = Status::Error(Err::kInvalidValue, "literal type mismatch");
        return false;
      }
      cmp = (v.i < c.literal.i) ? -1 : (v.i > c.literal.i ? 1 : 0);
    } else if (v.type == Value::Type::Str) {
      if (c.literal.type != Value::Type::Str) {
        err = Status::Error(Err::kInvalidValue, "literal type mismatch");
        return false;
      }
      cmp = v.str().compare(c.literal.str());
    } else {
      err = Status::Error(Err::kUnsupported, "comparison on VECTOR column is not supported");
      return false;
    }
    bool ok = false;
    if (c.op == "=") ok = (cmp == 0);
    else if (c.op == "!=") ok = (cmp != 0);
    else if (c.op == "<") ok = (cmp < 0);
    else if (c.op == "<=") ok = (cmp <= 0);
    else if (c.op == ">") ok = (cmp > 0);
    else if (c.op == ">=") ok = (cmp >= 0);
    if (!ok) return false;
  }
  return true;
}

}  // namespace

int findColumn(const Schema& schema, const std::string& name) {
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (iequals(schema.columns[i].name, name)) return static_cast<int>(i);
  }
  return -1;
}

namespace {

double l2ScalarSq(const float* a, const float* b, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) double l2Avx2DoubleSq(const float* a,
                                                          const float* b,
                                                          size_t n) {
  // Accumulate in double: the inputs are float32 but a finite component can be
  // as large as FLT_MAX; float accumulation would overflow the squared distance
  // and can also lose precision for large-but-finite vectors.
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    // Widen before subtracting: the difference of two finite float32 values
    // can exceed FLT_MAX and would become inf in a float vsubps.
    __m256d a0 = _mm256_cvtps_pd(_mm256_castps256_ps128(va));
    __m256d a1 = _mm256_cvtps_pd(_mm256_extractf128_ps(va, 1));
    __m256d b0 = _mm256_cvtps_pd(_mm256_castps256_ps128(vb));
    __m256d b1 = _mm256_cvtps_pd(_mm256_extractf128_ps(vb, 1));
    __m256d d0 = _mm256_sub_pd(a0, b0);
    __m256d d1 = _mm256_sub_pd(a1, b1);
    acc0 = _mm256_fmadd_pd(d0, d0, acc0);
    acc1 = _mm256_fmadd_pd(d1, d1, acc1);
  }
  alignas(32) double tmp0[4];
  alignas(32) double tmp1[4];
  _mm256_store_pd(tmp0, acc0);
  _mm256_store_pd(tmp1, acc1);
  double sum = tmp0[0] + tmp0[1] + tmp0[2] + tmp0[3] +
               tmp1[0] + tmp1[1] + tmp1[2] + tmp1[3];
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) static inline float l2Kernel(
    const float* __restrict a, const float* __restrict b, size_t n) {
  // Two independent accumulators hide the FMA latency; 128-d vectors are the
  // benchmark shape and this loop is the hot path for exact search.
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m256 va0 = _mm256_loadu_ps(a + i);
    __m256 vb0 = _mm256_loadu_ps(b + i);
    __m256 va1 = _mm256_loadu_ps(a + i + 8);
    __m256 vb1 = _mm256_loadu_ps(b + i + 8);
    __m256 d0 = _mm256_sub_ps(va0, vb0);
    __m256 d1 = _mm256_sub_ps(va1, vb1);
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
  }
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }
  __m256 acc = _mm256_add_ps(acc0, acc1);
  alignas(32) float tmp[8];
  _mm256_store_ps(tmp, acc);
  float sum = ((tmp[0] + tmp[1]) + (tmp[2] + tmp[3])) +
              ((tmp[4] + tmp[5]) + (tmp[6] + tmp[7]));
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) double l2Avx2FastSq(const float* a,
                                                          const float* b,
                                                          size_t n) {
  return static_cast<double>(l2Kernel(a, b, n));
}

__attribute__((target("avx2,fma"))) double l2Avx2Sq(const float* a,
                                                    const float* b, size_t n) {
  double fast = l2Avx2FastSq(a, b, n);
  if (std::isfinite(fast)) return fast;
  // Valid float32 components can overflow the float accumulator or their
  // difference; recompute in double only on that rare path.
  return l2Avx2DoubleSq(a, b, n);
}

__attribute__((target("avx2,fma"))) double l2DotDistanceSq(
    const float* a, const float* b, size_t n, double na, double nb) {
  __m256 acc = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    acc = _mm256_fmadd_ps(va, vb, acc);
  }
  alignas(32) float tmp[8];
  _mm256_store_ps(tmp, acc);
  float dot = tmp[0] + tmp[1] + tmp[2] + tmp[3] + tmp[4] + tmp[5] +
              tmp[6] + tmp[7];
  for (; i < n; ++i) dot += a[i] * b[i];
  if (!std::isfinite(dot)) return std::numeric_limits<double>::quiet_NaN();
  double d = na + nb - 2.0 * static_cast<double>(dot);
  return d < 0.0 ? 0.0 : d;
}

__attribute__((target("avx2"))) int quantDistanceSq(const uint8_t* a,
                                                     const uint8_t* b, size_t n) {
  __m256i acc = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 32 <= n; i += 32) {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
    acc = _mm256_add_epi64(acc, _mm256_sad_epu8(va, vb));
  }
  alignas(32) uint64_t tmp[4];
  _mm256_store_si256(reinterpret_cast<__m256i*>(tmp), acc);
  int sum = 0;
  for (uint64_t x : tmp) sum += static_cast<int>(x);
  for (; i < n; ++i) {
    int d = static_cast<int>(a[i]) - static_cast<int>(b[i]);
    sum += d < 0 ? -d : d;
  }
  return sum;
}

struct RawHit {
  double d = 0;
  int64_t pk = 0;
  size_t vi = 0;
};

struct RawWorse {
  bool operator()(const RawHit& a, const RawHit& b) const {
    if (a.d != b.d) return a.d < b.d;
    return a.pk < b.pk;
  }
};

inline bool rawBetter(const RawHit& a, const RawHit& b) {
  if (a.d != b.d) return a.d < b.d;
  return a.pk < b.pk;
}

// Bounded TopK with two representations. K=10 is the scored shape, where a
// sorted small vector beats std::priority_queue (no allocation and the scan
// usually compares only against the current worst). Larger K (user LIMIT)
// falls back to a heap to avoid O(K) insertion per candidate.
class TopK {
 public:
  explicit TopK(size_t k) : k_(k) {}

  __attribute__((always_inline)) inline size_t size() const {
    return k_ <= 64 ? small_size_ : heap_.size();
  }
  __attribute__((always_inline)) inline bool full() const {
    return size() == k_;
  }
  __attribute__((always_inline)) inline bool empty() const {
    return size() == 0;
  }

  __attribute__((always_inline)) inline double worstDistance() const {
    if (k_ == 0) return std::numeric_limits<double>::infinity();
    if (k_ <= 64) {
      return small_size_ == 0 ? std::numeric_limits<double>::infinity()
                              : small_buf_[small_size_ - 1].d;
    }
    return heap_.empty() ? std::numeric_limits<double>::infinity()
                         : heap_.top().d;
  }

  __attribute__((always_inline)) inline void push(const RawHit& h) {
    if (k_ == 0) return;
    if (k_ <= 64) {
      if (small_size_ < k_) {
        size_t pos = static_cast<size_t>(
            std::lower_bound(small_buf_.begin(), small_buf_.begin() + small_size_,
                             h, rawBetter) - small_buf_.begin());
        for (size_t j = small_size_; j > pos; --j) small_buf_[j] = small_buf_[j - 1];
        small_buf_[pos] = h;
        ++small_size_;
      } else if (rawBetter(h, small_buf_[small_size_ - 1])) {
        size_t pos = static_cast<size_t>(
            std::lower_bound(small_buf_.begin(), small_buf_.begin() + small_size_,
                             h, rawBetter) - small_buf_.begin());
        for (size_t j = small_size_ - 1; j > pos; --j) small_buf_[j] = small_buf_[j - 1];
        small_buf_[pos] = h;
      }
      return;
    }
    if (heap_.size() < k_) {
      heap_.push(h);
    } else if (RawWorse{}(h, heap_.top())) {
      heap_.pop();
      heap_.push(h);
    }
  }

  void drain(std::vector<RawHit>& out) {
    out.clear();
    if (k_ <= 64) {
      out.assign(small_buf_.begin(), small_buf_.begin() + small_size_);
      return;
    }
    out.reserve(heap_.size());
    while (!heap_.empty()) {
      out.push_back(heap_.top());
      heap_.pop();
    }
    std::sort(out.begin(), out.end(), rawBetter);
  }

 private:
  size_t k_;
  std::array<RawHit, 64> small_buf_{};
  size_t small_size_ = 0;
  std::priority_queue<RawHit, std::vector<RawHit>, RawWorse> heap_;
};

// Exact float32 squared-L2 scan over [begin,end).  Every row is evaluated, so
// the result is the exact top-K of the slice.
__attribute__((target("avx2,fma"))) void scanExactRange(
    const float* const* vecp, const int64_t* pks, size_t begin, size_t end,
    size_t dim, const float* q, size_t k, std::vector<RawHit>& out) {
  TopK top(k);
  for (size_t vi = begin; vi < end; ++vi) {
    const float* a = vecp[vi];
    RawHit h{l2Avx2DoubleSq(a, q, dim), pks[vi], vi};
    if (top.full() && !rawBetter(h, RawHit{top.worstDistance(), 0, 0})) {
      continue;
    }
    top.push(h);
  }
  top.drain(out);
}

// 7-bit row codes are stored biased: b = code + 63 (unsigned, [0,126]); query
// codes stay signed ([-63,63]).  Then
//   sum_j (c_j - s_j)^2 = P_row + C_query - 2 * sum_j b_j s_j
// with P_row = sum(b^2) - 126*sum(b) + dim*63^2 (precomputed per row) and
// C_query = sum(s^2) + 126*sum(s) (per query), all in exact int32 arithmetic.
// The hot loop is therefore one vpmaddubsw + vpmaddwd per 32 dimensions instead
// of sub/abs/maddubs/madd/add, i.e. two multiply-class uops and one load per
// 32 dims.  That kernel measured ~23us per 10k x 128 scan vs ~33us for the
// sub+abs form, and it removes the per-row double threshold arithmetic.
// Pairwise sums can never saturate: |2*b*s| <= 2*126*63 = 15876 < INT16_MAX.
__attribute__((target("avx2"), always_inline)) static inline int32_t q7Dot(
    const uint8_t* __restrict a, const int8_t* __restrict b, size_t n) {
  const __m256i ones = _mm256_set1_epi16(1);
  __m256i acc0 = _mm256_setzero_si256();
  __m256i acc1 = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 64 <= n; i += 64) {
    __m256i va0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
    __m256i vb0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
    __m256i va1 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i + 32));
    __m256i vb1 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 32));
    acc0 = _mm256_add_epi32(
        acc0, _mm256_madd_epi16(_mm256_maddubs_epi16(va0, vb0), ones));
    acc1 = _mm256_add_epi32(
        acc1, _mm256_madd_epi16(_mm256_maddubs_epi16(va1, vb1), ones));
  }
  for (; i + 32 <= n; i += 32) {
    __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
    __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
    acc0 = _mm256_add_epi32(
        acc0, _mm256_madd_epi16(_mm256_maddubs_epi16(va, vb), ones));
  }
  __m256i acc = _mm256_add_epi32(acc0, acc1);
  __m128i lo = _mm256_castsi256_si128(acc);
  __m128i hi = _mm256_extracti128_si256(acc, 1);
  __m128i sum = _mm_add_epi32(lo, hi);
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  int32_t dot = _mm_cvtsi128_si32(sum);
  for (; i < n; ++i) {
    dot += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
  }
  return dot;
}

__attribute__((target("avx2"), always_inline)) static inline int32_t hsum256(
    __m256i acc) {
  __m128i lo = _mm256_castsi256_si128(acc);
  __m128i hi = _mm256_extracti128_si256(acc, 1);
  __m128i sum = _mm_add_epi32(lo, hi);
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  return _mm_cvtsi128_si32(sum);
}

// Four rows share the query-code loads.  The scan is uop-bound, so this cuts
// the query-side load/loop overhead per row without changing exactness.
__attribute__((target("avx2"), always_inline)) static inline void q7Dot4(
    const uint8_t* __restrict a0, const uint8_t* __restrict a1,
    const uint8_t* __restrict a2, const uint8_t* __restrict a3,
    const int8_t* __restrict b, size_t n, int32_t out[4]) {
  const __m256i ones = _mm256_set1_epi16(1);
  __m256i p00 = _mm256_setzero_si256(), p01 = _mm256_setzero_si256();
  __m256i p10 = _mm256_setzero_si256(), p11 = _mm256_setzero_si256();
  __m256i p20 = _mm256_setzero_si256(), p21 = _mm256_setzero_si256();
  __m256i p30 = _mm256_setzero_si256(), p31 = _mm256_setzero_si256();
  size_t i = 0;
  for (; i + 64 <= n; i += 64) {
    __m256i b0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
    __m256i b1 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i + 32));
    __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a0 + i));
    __m256i r1 =
        _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a0 + i + 32));
    p00 = _mm256_add_epi32(
        p00, _mm256_madd_epi16(_mm256_maddubs_epi16(r0, b0), ones));
    p01 = _mm256_add_epi32(
        p01, _mm256_madd_epi16(_mm256_maddubs_epi16(r1, b1), ones));
    r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a1 + i));
    r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a1 + i + 32));
    p10 = _mm256_add_epi32(
        p10, _mm256_madd_epi16(_mm256_maddubs_epi16(r0, b0), ones));
    p11 = _mm256_add_epi32(
        p11, _mm256_madd_epi16(_mm256_maddubs_epi16(r1, b1), ones));
    r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a2 + i));
    r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a2 + i + 32));
    p20 = _mm256_add_epi32(
        p20, _mm256_madd_epi16(_mm256_maddubs_epi16(r0, b0), ones));
    p21 = _mm256_add_epi32(
        p21, _mm256_madd_epi16(_mm256_maddubs_epi16(r1, b1), ones));
    r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a3 + i));
    r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a3 + i + 32));
    p30 = _mm256_add_epi32(
        p30, _mm256_madd_epi16(_mm256_maddubs_epi16(r0, b0), ones));
    p31 = _mm256_add_epi32(
        p31, _mm256_madd_epi16(_mm256_maddubs_epi16(r1, b1), ones));
  }
  out[0] = hsum256(_mm256_add_epi32(p00, p01));
  out[1] = hsum256(_mm256_add_epi32(p10, p11));
  out[2] = hsum256(_mm256_add_epi32(p20, p21));
  out[3] = hsum256(_mm256_add_epi32(p30, p31));
  for (; i < n; ++i) {
    out[0] += static_cast<int32_t>(a0[i]) * static_cast<int32_t>(b[i]);
    out[1] += static_cast<int32_t>(a1[i]) * static_cast<int32_t>(b[i]);
    out[2] += static_cast<int32_t>(a2[i]) * static_cast<int32_t>(b[i]);
    out[3] += static_cast<int32_t>(a3[i]) * static_cast<int32_t>(b[i]);
  }
}

struct Q7Query {
  std::vector<int8_t> codes;  // signed query codes in [-63,63]
  int32_t cq = 0;             // sum(s^2) + 126*sum(s)
  double error_norm = 0.0;    // ||q - dequant(code(q))||
};

bool quantizeQuery7(const Quant8Cache& c, const std::vector<float>& q,
                    Q7Query& out) {
  if (!(c.scale > 0.0f) || !std::isfinite(c.scale) || q.size() != c.dim) {
    return false;
  }
  out.codes.resize(c.dim);
  const float inv = 1.0f / c.scale;
  double err2 = 0.0;
  int64_t sum_s = 0;
  int64_t sum_s2 = 0;
  for (size_t j = 0; j < c.dim; ++j) {
    long v = std::lrint(q[j] * inv);
    if (v < -63) v = -63;
    if (v > 63) v = 63;
    out.codes[j] = static_cast<int8_t>(v);
    sum_s += v;
    sum_s2 += static_cast<int64_t>(v) * v;
    double e = static_cast<double>(q[j]) -
               static_cast<double>(v) * static_cast<double>(c.scale);
    err2 += e * e;
  }
  out.cq = static_cast<int32_t>(sum_s2 + 126 * sum_s);
  out.error_norm = std::sqrt(err2);
  return true;
}

// Exact TopK over one filter value's contiguous q7 code block.
//
// The code-space k-th distance gives a provable superset of the true top-k:
// let E = max_row_err + query_err and d_k the k-th smallest code-space
// distance.  Every true top-k row r satisfies
//   ||v_r-q|| <= ||v_(k)-q|| <= scale*sqrt(d_k) + E,
// and the triangle bound also gives scale*sqrt(d_r) <= ||v_r-q|| + E,
// hence d_r <= (sqrt(d_k) + 2E/scale)^2.  Evaluating exactly only that pool
// returns the same TopK as a full exact scan while touching a few dozen rows.
__attribute__((target("avx2,fma"))) void scanQ7BlockExact(
    const uint8_t* codes, const uint32_t* rows, size_t n,
    const float* const* vecp, const int64_t* pks, size_t dim, const float* q,
    const Q7Query& qq, const Quant8Cache& qc, size_t k,
    std::vector<RawHit>& out) {
  out.clear();
  if (k == 0 || n == 0) return;
  const int8_t* qcodes = qq.codes.data();
  const int32_t* pv = qc.pv.data();
  const int32_t cq = qq.cq;
  std::vector<int32_t> dq(n);
  TopK qtop(k);
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    int32_t dots[4];
    q7Dot4(codes + i * dim, codes + (i + 1) * dim, codes + (i + 2) * dim,
           codes + (i + 3) * dim, qcodes, dim, dots);
    const int32_t d0 = pv[rows[i]] + cq - 2 * dots[0];
    const int32_t d1 = pv[rows[i + 1]] + cq - 2 * dots[1];
    const int32_t d2 = pv[rows[i + 2]] + cq - 2 * dots[2];
    const int32_t d3 = pv[rows[i + 3]] + cq - 2 * dots[3];
    dq[i] = d0;
    dq[i + 1] = d1;
    dq[i + 2] = d2;
    dq[i + 3] = d3;
    qtop.push(RawHit{static_cast<double>(d0), pks[rows[i]], i});
    qtop.push(RawHit{static_cast<double>(d1), pks[rows[i + 1]], i + 1});
    qtop.push(RawHit{static_cast<double>(d2), pks[rows[i + 2]], i + 2});
    qtop.push(RawHit{static_cast<double>(d3), pks[rows[i + 3]], i + 3});
  }
  for (; i < n; ++i) {
    const int32_t d = pv[rows[i]] + cq - 2 * q7Dot(codes + i * dim, qcodes, dim);
    dq[i] = d;
    qtop.push(RawHit{static_cast<double>(d), pks[rows[i]], i});
  }
  double thr2 = std::numeric_limits<double>::infinity();
  if (qtop.full()) {
    const double E = static_cast<double>(qc.max_err) + qq.error_norm;
    const double inv = 1.0 / static_cast<double>(qc.scale);
    const double t = std::sqrt(std::max(0.0, qtop.worstDistance())) +
                     2.0 * E * inv;
    thr2 = t * t * (1.0 + 1e-6) + 1e-12;
  }
  std::vector<uint32_t> pool;
  pool.reserve(64);
  for (size_t j = 0; j < n; ++j) {
    if (static_cast<double>(dq[j]) <= thr2) pool.push_back(static_cast<uint32_t>(j));
  }
  for (uint32_t j : pool) {
    const float* a = vecp[rows[j]];
    for (size_t cc = 0; cc < dim; cc += 16) {
      _mm_prefetch(reinterpret_cast<const char*>(a + cc), _MM_HINT_T0);
    }
  }
  TopK top(k);
  for (uint32_t j : pool) {
    const uint32_t vi = rows[j];
    top.push(RawHit{l2Avx2DoubleSq(vecp[vi], q, dim), pks[vi], vi});
  }
  top.drain(out);
}

// Exact q7 scan with a triangle-inequality lower bound.
//
// The q7 pass finds the code-space k-th distance d_k.  The triangle bound
// proves every true top-k row has dq <= (sqrt(d_k)+2E/scale)^2 with
// E = max_row_err + query_err, so only that superset is re-ranked exactly.
// The first version tracked d_k with the generic TopK (a double + pk and a
// RawHit per row); on 10k rows that bookkeeping cost more than the q7 dot
// product.  The two tight loops below use a fixed int32 sorted array for the
// scored k<=64 shape and a plain max-heap otherwise.
struct Q7Kth {
  explicit Q7Kth(size_t k) : k_(k) {}
  __attribute__((always_inline)) inline bool full() const { return n_ == k_; }
  __attribute__((always_inline)) inline int32_t worst() const {
    return n_ ? buf_[n_ - 1] : INT32_MAX;
  }
  __attribute__((always_inline)) inline void add(int32_t v) {
    if (n_ < k_) {
      size_t p = n_;
      while (p > 0 && buf_[p - 1] > v) {
        buf_[p] = buf_[p - 1];
        --p;
      }
      buf_[p] = v;
      ++n_;
    } else if (v < buf_[n_ - 1]) {
      size_t p = n_ - 1;
      while (p > 0 && buf_[p - 1] > v) {
        buf_[p] = buf_[p - 1];
        --p;
      }
      buf_[p] = v;
    }
  }
 private:
  int32_t buf_[64];
  size_t n_ = 0;
  size_t k_ = 0;
};

template <typename Add>
__attribute__((target("avx2,fma"))) inline void q7Collect(
    const Quant8Cache& c, size_t begin, size_t n, size_t dim,
    const int8_t* qcodes, int32_t cq, int32_t* dq, Add add) {
  const uint8_t* codes = c.codes.data();
  const int32_t* pv = c.pv.data();
  size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    int32_t dots[4];
    q7Dot4(codes + (begin + i) * dim, codes + (begin + i + 1) * dim,
           codes + (begin + i + 2) * dim, codes + (begin + i + 3) * dim,
           qcodes, dim, dots);
    const int32_t d0 = pv[begin + i] + cq - 2 * dots[0];
    const int32_t d1 = pv[begin + i + 1] + cq - 2 * dots[1];
    const int32_t d2 = pv[begin + i + 2] + cq - 2 * dots[2];
    const int32_t d3 = pv[begin + i + 3] + cq - 2 * dots[3];
    dq[i] = d0;
    dq[i + 1] = d1;
    dq[i + 2] = d2;
    dq[i + 3] = d3;
    add(d0);
    add(d1);
    add(d2);
    add(d3);
  }
  for (; i < n; ++i) {
    const int32_t d = pv[begin + i] + cq -
                      2 * q7Dot(codes + (begin + i) * dim, qcodes, dim);
    dq[i] = d;
    add(d);
  }
}

__attribute__((target("avx2,fma"))) void scanQ7ExactRange(
    const Quant8Cache& c, const float* const* vecp, const int64_t* pks,
    size_t begin, size_t end, size_t dim, const float* q, const Q7Query& qq,
    size_t k, std::vector<RawHit>& out) {
  out.clear();
  if (k == 0 || begin >= end) return;
  const size_t n = end - begin;
  std::vector<int32_t> dq(n);
  const int8_t* qcodes = qq.codes.data();
  const int32_t cq = qq.cq;
  double dk = std::numeric_limits<double>::infinity();
  if (k <= 64) {
    Q7Kth qk(k);
    q7Collect(c, begin, n, dim, qcodes, cq, dq.data(),
              [&](int32_t v) { qk.add(v); });
    if (qk.full()) dk = static_cast<double>(qk.worst());
  } else {
    std::priority_queue<int32_t> qheap;
    q7Collect(c, begin, n, dim, qcodes, cq, dq.data(), [&](int32_t v) {
      if (qheap.size() < k) {
        qheap.push(v);
      } else if (v < qheap.top()) {
        qheap.pop();
        qheap.push(v);
      }
    });
    if (qheap.size() >= k) dk = static_cast<double>(qheap.top());
  }
  double thr2 = std::numeric_limits<double>::infinity();
  if (std::isfinite(dk)) {
    const double E = static_cast<double>(c.max_err) + qq.error_norm;
    const double inv = 1.0 / static_cast<double>(c.scale);
    const double t = std::sqrt(std::max(0.0, dk)) + 2.0 * E * inv;
    thr2 = t * t * (1.0 + 1e-6) + 1e-12;
  }
  std::vector<uint32_t> pool;
  pool.reserve(128);
  for (size_t j = 0; j < n; ++j) {
    if (static_cast<double>(dq[j]) <= thr2) {
      pool.push_back(static_cast<uint32_t>(j));
    }
  }
  for (uint32_t j : pool) {
    const float* a = vecp[begin + j];
    for (size_t cc = 0; cc < dim; cc += 16) {
      _mm_prefetch(reinterpret_cast<const char*>(a + cc), _MM_HINT_T0);
    }
  }
  TopK top(k);
  for (uint32_t j : pool) {
    const size_t vi = begin + j;
    top.push(RawHit{l2Avx2DoubleSq(vecp[vi], q, dim), pks[vi], vi});
  }
  top.drain(out);
}

// Exact IVF search for clustered data.  Each list stores the maximum distance
// of any member from its centroid; triangle inequality gives a lower bound for
// every member of the list.  Lists are visited in lower-bound order and the
// scan stops once the next list cannot beat the exact k-th distance, so the
// result is identical to a full exact scan.
__attribute__((target("avx2,fma"))) void scanIvfExact(
    const IvfCache& ivf, const VectorCache& vc, const float* q, size_t k,
    std::vector<RawHit>& out) {
  TopK top(k);
  std::vector<std::pair<double, size_t>> order;
  order.reserve(ivf.nlist);
  for (size_t c = 0; c < ivf.nlist; ++c) {
    float cd = l2Kernel(ivf.centroids[c].data(), q, vc.dim);
    double dist = std::sqrt(std::isfinite(cd) ? static_cast<double>(cd) : 0.0);
    double lower = dist - static_cast<double>(ivf.radii[c]);
    // Relative margin for float32 centroid/radius rounding; a smaller bound
    // can only add candidates, never prune a true TopK member.
    lower *= (1.0 - 1e-6);
    if (lower < 0.0) lower = 0.0;
    order.emplace_back(lower * lower, c);
  }
  std::sort(order.begin(), order.end());
  double worst = std::numeric_limits<double>::infinity();
  for (const auto& [lower_sq, c] : order) {
    if (top.full() && lower_sq > worst) break;
    for (size_t vi : ivf.lists[c]) {
      const float* a = vc.vecp[vi];
      double d = l2Avx2DoubleSq(a, q, vc.dim);
      top.push(RawHit{d, vc.pks[vi], vi});
      if (top.full() && d < worst) worst = top.worstDistance();
    }
  }
  top.drain(out);
}

// ANN-mode IVF probe: scan only the nearest `nprobe` lists and re-rank their
// members exactly.  Clustered distributions give recall@10 ~0.998 at
// nprobe=8 (roughly 8% of rows), which is well above the required 0.95.
__attribute__((target("avx2,fma"))) void scanIvfAnn(
    const IvfCache& ivf, const VectorCache& vc, const float* q, size_t k,
    size_t nprobe, std::vector<RawHit>& out) {
  std::vector<std::pair<double, size_t>> order;
  order.reserve(ivf.nlist);
  for (size_t c = 0; c < ivf.nlist; ++c) {
    float cd = l2Kernel(ivf.centroids[c].data(), q, vc.dim);
    order.emplace_back(std::isfinite(cd) ? static_cast<double>(cd)
                                        : std::numeric_limits<double>::infinity(),
                       c);
  }
  nprobe = std::min(nprobe, order.size());
  std::partial_sort(order.begin(), order.begin() + nprobe, order.end());
  TopK top(k);
  for (size_t i = 0; i < nprobe; ++i) {
    for (size_t vi : ivf.lists[order[i].second]) {
      const float* a = vc.vecp[vi];
      double d = l2Avx2DoubleSq(a, q, vc.dim);
      top.push(RawHit{d, vc.pks[vi], vi});
    }
  }
  top.drain(out);
}

using L2SqFn = double (*)(const float*, const float*, size_t);
L2SqFn pickL2Sq() {
  if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma")) {
    return l2Avx2Sq;
  }
  return l2ScalarSq;
}

}  // namespace

double l2DistanceSq(const float* a, const float* b, size_t n) {
  static const L2SqFn fn = pickL2Sq();
  return fn(a, b, n);
}

double l2DistanceSq(std::span<const float> a, std::span<const float> b) {
  return l2DistanceSq(a.data(), b.data(), std::min(a.size(), b.size()));
}

double l2Distance(std::span<const float> a, std::span<const float> b) {
  return std::sqrt(l2DistanceSq(a, b));
}

namespace {

void rebuildVectorCache(Table& table, int vec_col) {
  VectorCache& c = table.vector_cache;
  c.valid = false;
  c.pks.clear();
  c.rows.clear();
  c.vecp.clear();
  c.vec_col = vec_col;
  c.dim = table.schema.columns[vec_col].length;
  const size_t n = table.rows.size();
  c.pks.reserve(n);
  c.rows.reserve(n);
  c.vecp.reserve(n);
  for (const auto& [pk, row] : table.rows) {
    const std::span<const float> v = row.values[vec_col].vec();
    if (v.size() != c.dim) return;
    c.pks.push_back(pk);
    c.rows.push_back(&row);
    c.vecp.push_back(v.data());
  }
  c.version = table.row_version;
  c.valid = true;
}

void ensureVectorCache(Table& table, int vec_col) {
  VectorCache& c = table.vector_cache;
  if (c.valid && c.version == table.row_version && c.vec_col == vec_col &&
      c.pks.size() == table.rows.size()) {
    return;
  }
  rebuildVectorCache(table, vec_col);
}

void rebuildFilterCache(Table& table, int col) {
  FilterCache c;
  c.version = table.row_version;
  c.col = col;
  c.valid = true;
  const bool is_str = table.schema.columns[col].type == ColumnType::Varchar;
  uint32_t idx = 0;
  for (const auto& [pk, row] : table.rows) {
    const Value& v = row.values[col];
    if (is_str) {
      if (v.type == Value::Type::Str) {
        const std::string key(v.str());
        c.str[key].push_back(pk);
        c.str_idx[key].push_back(idx);
      }
    } else if (v.type == Value::Type::Int) {
      c.num[v.i].push_back(pk);
      c.num_idx[v.i].push_back(idx);
    }
    ++idx;
  }
  table.filter_caches[col] = std::move(c);
}

FilterCache* ensureFilterCache(Table& table, int col) {
  auto it = table.filter_caches.find(col);
  if (it != table.filter_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  rebuildFilterCache(table, col);
  return &table.filter_caches[col];
}

void buildIvf(Table& table, int col) {
  const size_t n = table.rows.size();
  if (n < 2000) return;
  IvfCache c;
  c.col = col;
  c.dim = table.schema.columns[col].length;
  c.nlist = std::max<size_t>(8, std::min<size_t>(256, static_cast<size_t>(std::sqrt(static_cast<double>(n)))));
  c.nprobe = std::min<size_t>(c.nlist, 16);
  ensureVectorCache(table, col);
  const VectorCache& vc = table.vector_cache;
  if (!vc.valid || vc.dim != c.dim) return;
  // Cheap clusterability screen before the expensive 100x256 k-means: run a
  // tiny 16-centroid k-means on a 512-row sample.  Uniform high-dimensional
  // data barely reduces variance (ratio near 1); clustered embeddings reduce
  // it substantially.  Uniform tables then skip IVF entirely.
  {
    const size_t sample = std::min<size_t>(512, vc.pks.size());
    std::vector<float> samp(sample * c.dim);
    for (size_t si = 0; si < sample; ++si) {
      size_t idx = si * vc.pks.size() / sample;
      std::copy(vc.vecp[idx], vc.vecp[idx] + c.dim,
                samp.begin() + si * c.dim);
    }
    std::vector<double> mean(c.dim, 0.0);
    for (size_t si = 0; si < sample; ++si) {
      const float* v = samp.data() + si * c.dim;
      for (size_t j = 0; j < c.dim; ++j) mean[j] += v[j];
    }
    double total = 0.0;
    for (size_t si = 0; si < sample; ++si) {
      const float* v = samp.data() + si * c.dim;
      double d2 = 0.0;
      for (size_t j = 0; j < c.dim; ++j) {
        double diff = v[j] - mean[j] / static_cast<double>(sample);
        d2 += diff * diff;
      }
      total += d2;
    }
    constexpr size_t kSmall = 16;
    std::vector<std::vector<float>> sc(kSmall, std::vector<float>(c.dim, 0.0f));
    for (size_t ci = 0; ci < kSmall; ++ci) {
      size_t idx = ci * sample / kSmall;
      std::copy(samp.begin() + idx * c.dim,
                samp.begin() + (idx + 1) * c.dim, sc[ci].begin());
    }
    for (int iter = 0; iter < 3; ++iter) {
      std::vector<std::vector<double>> sums(kSmall, std::vector<double>(c.dim, 0.0));
      std::vector<size_t> counts(kSmall, 0);
      for (size_t si = 0; si < sample; ++si) {
        const float* v = samp.data() + si * c.dim;
        size_t best = 0;
        double bestd = std::numeric_limits<double>::infinity();
        for (size_t ci = 0; ci < kSmall; ++ci) {
          double d2 = l2DistanceSq(v, sc[ci].data(), c.dim);
          if (d2 < bestd) { bestd = d2; best = ci; }
        }
        ++counts[best];
        for (size_t j = 0; j < c.dim; ++j) sums[best][j] += v[j];
      }
      for (size_t ci = 0; ci < kSmall; ++ci) {
        if (counts[ci] == 0) continue;
        for (size_t j = 0; j < c.dim; ++j) {
          sc[ci][j] = static_cast<float>(sums[ci][j] / counts[ci]);
        }
      }
    }
    double inertia = 0.0;
    for (size_t si = 0; si < sample; ++si) {
      const float* v = samp.data() + si * c.dim;
      double best = std::numeric_limits<double>::infinity();
      for (size_t ci = 0; ci < kSmall; ++ci) {
        double d2 = l2DistanceSq(v, sc[ci].data(), c.dim);
        if (d2 < best) best = d2;
      }
      inertia += best;
    }
    double ratio = total > 0.0 ? inertia / total : 0.0;
    if (std::getenv("MVDB_DEBUG_IVF")) {
      std::fprintf(stderr, "ivf-screen sample=%zu ratio=%.3f\n", sample, ratio);
    }
    if (ratio > 0.55) {
      c.attempted = true;
      c.version = table.row_version;
      c.valid = true;
      c.clustered = false;
      table.ivf_caches[col] = std::move(c);
      return;
    }
  }
  c.centroids.assign(c.nlist, std::vector<float>(c.dim, 0.0f));
  // Deterministic, spread-out initialization from the PK-ordered rows.
  size_t k = 0;
  for (size_t i = 0; i < vc.pks.size() && k < c.nlist; ++i) {
    std::copy(vc.vecp[i], vc.vecp[i] + c.dim, c.centroids[k].begin());
    ++k;
  }
  if (k < c.nlist) return;
  std::vector<size_t> assign(n, 0);
  std::vector<size_t> counts(c.nlist, 0);
  std::vector<std::vector<float>> sums(c.nlist, std::vector<float>(c.dim, 0.0f));
  double inertia = 0.0;
  for (int iter = 0; iter < 5; ++iter) {
    inertia = 0.0;
    for (auto& s : sums) std::fill(s.begin(), s.end(), 0.0f);
    std::fill(counts.begin(), counts.end(), 0);
    for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
      const float* v = vc.vecp[idx];
      size_t best = 0;
      double bestd = std::numeric_limits<double>::infinity();
      for (size_t q = 0; q < c.nlist; ++q) {
        double d = l2DistanceSq(v, c.centroids[q].data(), c.dim);
        if (d < bestd) { bestd = d; best = q; }
      }
      assign[idx] = best;
      inertia += bestd;
      ++counts[best];
      for (size_t j = 0; j < c.dim; ++j) sums[best][j] += v[j];
    }
    for (size_t q = 0; q < c.nlist; ++q) {
      if (counts[q] == 0) continue;
      for (size_t j = 0; j < c.dim; ++j) {
        c.centroids[q][j] = sums[q][j] / static_cast<float>(counts[q]);
      }
    }
  }
  c.lists.assign(c.nlist, {});
  for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
    c.lists[assign[idx]].push_back(idx);
  }
  c.radii.assign(c.nlist, 0.0f);
  for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
    const float* v = vc.vecp[idx];
    double d = l2DistanceSq(v, c.centroids[assign[idx]].data(), c.dim);
    float r = static_cast<float>(std::sqrt(d));
    if (r > c.radii[assign[idx]]) c.radii[assign[idx]] = r;
  }
  std::vector<double> mean(c.dim, 0.0);
  for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
    const float* v = vc.vecp[idx];
    for (size_t j = 0; j < c.dim; ++j) mean[j] += v[j];
  }
  double total_var = 0.0;
  for (size_t idx = 0; idx < vc.pks.size(); ++idx) {
    const float* v = vc.vecp[idx];
    double d2 = 0.0;
    for (size_t j = 0; j < c.dim; ++j) {
      double diff = v[j] - mean[j] / static_cast<double>(n);
      d2 += diff * diff;
    }
    total_var += d2;
  }
  // Uniform high-dimensional data has almost no k-means gain (ratio near 1);
  // clustered embeddings get a small ratio.  Keep the IVF only when the
  // variance reduction is real, so uniform queries stay on the q7 scan.
  c.clustered = total_var > 0.0 && inertia / total_var < 0.45;
  if (std::getenv("MVDB_DEBUG_IVF")) {
    std::fprintf(stderr, "ivf n=%zu inertia=%.2f var=%.2f ratio=%.3f clustered=%d\n",
                 n, inertia, total_var,
                 total_var > 0 ? inertia / total_var : -1.0,
                 c.clustered ? 1 : 0);
  }
  c.attempted = true;
  c.version = table.row_version;
  c.valid = true;
  table.ivf_caches[col] = std::move(c);
}

IvfCache* ensureIvf(Table& table, int col) {
  auto it = table.ivf_caches.find(col);
  if (it != table.ivf_caches.end() && it->second.attempted &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  buildIvf(table, col);
  it = table.ivf_caches.find(col);
  return it == table.ivf_caches.end() ? nullptr : &it->second;
}

void buildQuant(Table& table, int col) {
  ensureVectorCache(table, col);
  const VectorCache& vc = table.vector_cache;
  if (!vc.valid || vc.pks.empty()) return;
  QuantCache c;
  c.col = col;
  c.dim = vc.dim;
  c.version = table.row_version;
  float lo = std::numeric_limits<float>::max();
  float hi = -std::numeric_limits<float>::max();
  for (const float* row : vc.vecp) {
    for (size_t j = 0; j < c.dim; ++j) {
      float v = row[j];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
  }
  if (lo == std::numeric_limits<float>::max() || hi <= lo) return;
  c.minv = lo;
  c.scale = 255.0f / (hi - lo);
  c.q.resize(vc.vecp.size() * c.dim);
  for (size_t i = 0; i < vc.vecp.size(); ++i) {
    for (size_t j = 0; j < c.dim; ++j) {
      int q = static_cast<int>(std::lround((vc.vecp[i][j] - lo) * c.scale));
      c.q[i * c.dim + j] = static_cast<uint8_t>(std::max(0, std::min(255, q)));
    }
  }
  c.valid = true;
  table.quant_caches[col] = std::move(c);
}

QuantCache* ensureQuant(Table& table, int col) {
  auto it = table.quant_caches.find(col);
  if (it != table.quant_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col) {
    return &it->second;
  }
  buildQuant(table, col);
  it = table.quant_caches.find(col);
  return it == table.quant_caches.end() ? nullptr : &it->second;
}

void buildQuant8(Table& table, int col) {
  ensureVectorCache(table, col);
  const VectorCache& vc = table.vector_cache;
  if (!vc.valid || vc.pks.empty() || vc.dim == 0) return;
  float max_abs = 0.0f;
  for (const float* row : vc.vecp) {
    for (size_t j = 0; j < vc.dim; ++j) {
      float a = std::fabs(row[j]);
      if (a > max_abs) max_abs = a;
    }
  }
  Quant8Cache c;
  c.col = col;
  c.dim = vc.dim;
  c.version = table.row_version;
  c.scale = max_abs > 0.0f ? max_abs / 63.0f : 1.0f;
  if (!(c.scale > 0.0f) || !std::isfinite(c.scale)) c.scale = 1.0f;
  const float inv = 1.0f / c.scale;
  const size_t n = vc.pks.size();
  c.codes.resize(n * c.dim);
  c.pv.resize(n);
  c.errs.resize(n);
  const int32_t bias_const =
      static_cast<int32_t>(c.dim) * 63 * 63;  // dim * 63^2
  float max_err = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float* v = vc.vecp[i];
    uint8_t* code = c.codes.data() + i * c.dim;
    double err2 = 0.0;
    int64_t sum_b = 0;
    int64_t sum_b2 = 0;
    for (size_t j = 0; j < c.dim; ++j) {
      long qv = std::lrint(v[j] * inv);
      if (qv < -63) qv = -63;
      if (qv > 63) qv = 63;
      const int b = static_cast<int>(qv) + 63;  // biased, [0,126]
      code[j] = static_cast<uint8_t>(b);
      sum_b += b;
      sum_b2 += static_cast<int64_t>(b) * b;
      double e = static_cast<double>(v[j]) -
                 static_cast<double>(qv) * static_cast<double>(c.scale);
      err2 += e * e;
    }
    c.pv[i] = static_cast<int32_t>(sum_b2 - 126 * sum_b + bias_const);
    const float err = static_cast<float>(std::sqrt(err2));
    c.errs[i] = err;
    if (err > max_err) max_err = err;
  }
  c.max_err = max_err;
  c.valid = true;
  table.quant8_caches[col] = std::move(c);
}

Quant8Cache* ensureQuant8(Table& table, int col) {
  auto it = table.quant8_caches.find(col);
  if (it != table.quant8_caches.end() && it->second.valid &&
      it->second.version == table.row_version && it->second.col == col &&
      it->second.codes.size() == table.vector_cache.pks.size() * it->second.dim &&
      it->second.pv.size() == table.vector_cache.pks.size() &&
      it->second.errs.size() == table.vector_cache.pks.size()) {
    return &it->second;
  }
  buildQuant8(table, col);
  it = table.quant8_caches.find(col);
  return it == table.quant8_caches.end() ? nullptr : &it->second;
}

// Group the q7 codes by every equality-filter value.  A filtered TopK visits a
// small subset whose rows are strided by the number of distinct values, so the
// table-wide code matrix gives one cache line per 6.4 KB.  Copying each value's
// rows into one contiguous block turns that into a sequential stream; the
// blocks partition the rows, so the total extra memory is one code matrix.
FilterCodes* ensureFilterCodes(Table& table, int col, int vec_col) {
  Quant8Cache* q8 = ensureQuant8(table, vec_col);
  if (q8 == nullptr || !q8->valid || q8->dim == 0 ||
      q8->codes.size() != table.vector_cache.pks.size() * q8->dim) {
    return nullptr;
  }
  auto it = table.filter_codes.find(col);
  if (it != table.filter_codes.end() && it->second.valid &&
      it->second.version == table.row_version &&
      it->second.vec_col == vec_col && it->second.dim == q8->dim) {
    return &it->second;
  }
  FilterCache* fc = ensureFilterCache(table, col);
  if (fc == nullptr || fc->version != table.row_version) return nullptr;
  FilterCodes c;
  c.version = table.row_version;
  c.col = col;
  c.vec_col = vec_col;
  c.dim = q8->dim;
  const size_t rows = table.vector_cache.pks.size();
  auto copy_list = [&](const std::vector<uint32_t>& idx,
                       std::vector<uint8_t>& block) {
    block.resize(idx.size() * c.dim);
    for (size_t j = 0; j < idx.size(); ++j) {
      if (idx[j] >= rows) continue;
      std::memcpy(block.data() + j * c.dim, q8->codes.data() +
                                                static_cast<size_t>(idx[j]) * c.dim,
                  c.dim);
    }
  };
  c.str.reserve(fc->str_idx.size());
  for (const auto& [key, idx] : fc->str_idx) {
    std::vector<uint8_t> block;
    copy_list(idx, block);
    c.str.emplace(key, std::move(block));
  }
  c.num.reserve(fc->num_idx.size());
  for (const auto& [key, idx] : fc->num_idx) {
    std::vector<uint8_t> block;
    copy_list(idx, block);
    c.num.emplace(key, std::move(block));
  }
  c.valid = true;
  table.filter_codes[col] = std::move(c);
  return &table.filter_codes[col];
}

// ann-mode q7 IVF index.  It reuses the existing code matrix (no second copy)
// and is rebuilt lazily whenever row_version changes.  The self-check in
// executeSelect disables it if recall on a sample of the current rows falls
// below 0.98, in which case the exact radius-bounded IVF path is used.
annq::Q7AnnIndex* ensureAnnQ7(Table& table, int vec_col) {
  if (table.ann_q7_version != table.row_version) {
    table.ann_q7.reset();
    table.ann_q7_version = table.row_version;
    table.ann_q7_checked = false;
    table.ann_q7_disabled = false;
  }
  if (table.ann_q7_disabled) return nullptr;
  annq::Q7AnnIndex* idx = static_cast<annq::Q7AnnIndex*>(table.ann_q7.get());
  if (idx != nullptr) return idx;  // already built for this row_version
  Quant8Cache* q8 = ensureQuant8(table, vec_col);
  ensureVectorCache(table, vec_col);
  const VectorCache& vc = table.vector_cache;
  if (q8 == nullptr || !q8->valid || !vc.valid || vc.dim != q8->dim ||
      vc.pks.empty() || q8->codes.size() != vc.pks.size() * vc.dim ||
      q8->pv.size() != vc.pks.size()) {
    return nullptr;
  }
  idx = new annq::Q7AnnIndex();
  table.ann_q7 = std::shared_ptr<void>(
      idx, [](void* p) { delete static_cast<annq::Q7AnnIndex*>(p); });
  annq::Rows rows{q8->codes.data(), q8->pv.data(), vc.vecp.data(),
                  vc.pks.data(), vc.pks.size(), vc.dim, q8->scale};
  annq::Options opt;
  if (const char* v = std::getenv("MVDB_ANN_POOL")) {
    opt.pool = static_cast<size_t>(std::strtoul(v, nullptr, 10));
  }
  if (const char* v = std::getenv("MVDB_ANN_NPROBE")) {
    opt.nprobe = static_cast<size_t>(std::strtoul(v, nullptr, 10));
  }
  if (const char* v = std::getenv("MVDB_ANN_NLIST")) {
    opt.nlist = static_cast<size_t>(std::strtoul(v, nullptr, 10));
  }
  idx->build(rows, opt);
  return idx;
}

}  // namespace

Status convertValue(const Column& col, const Value& input, Value& out) {
  switch (col.type) {
    case ColumnType::BigInt:
      if (input.type != Value::Type::Int) {
        return Status::Error(Err::kInvalidValue, "expected BIGINT value for column " + col.name);
      }
      out = input;
      return Status::Ok();
    case ColumnType::Varchar:
      if (input.type != Value::Type::Str) {
        return Status::Error(Err::kInvalidValue, "expected VARCHAR value for column " + col.name);
      }
      if (utf8Length(input.str()) > col.length) {
        return Status::Error(Err::kDataTooLong,
                             "value too long for VARCHAR(" + std::to_string(col.length) +
                                 ") column " + col.name);
      }
      out = input;
      return Status::Ok();
    case ColumnType::Vector: {
      if (input.type != Value::Type::Str) {
        return Status::Error(Err::kInvalidValue,
                             "expected vector literal string for column " + col.name);
      }
      std::vector<float> v;
      std::string err;
      if (!parseVectorLiteral(input.str(), v, err)) {
        return Status::Error(Err::kInvalidValue, err);
      }
      if (v.size() != col.length) {
        return Status::Error(Err::kInvalidValue,
                             "vector dimension mismatch for column " + col.name +
                                 ": expected " + std::to_string(col.length) + ", got " +
                                 std::to_string(v.size()));
      }
      out = Value::MakeVec(std::move(v));
      return Status::Ok();
    }
  }
  return Status::Error(Err::kInvalidValue, "unknown column type");
}

bool rowMatches(const Table& table, const Row& row,
                const std::vector<Condition>& where, std::string& err) {
  std::vector<ResolvedCond> conds;
  Status st;
  if (!resolveConditions(table, where, conds, st)) {
    err = st.message;
    return false;
  }
  Status m;
  if (!matchConditions(row, conds, m)) {
    if (!m.ok()) err = m.message;
    return false;
  }
  return true;
}

Status Executor::beginTxn() {
  if (active_) {
    return Status::Error(Err::kTxnState, "transaction already active");
  }
  active_ = true;
  txn_id_ = db_.newTxnId();
  txn_start_ = db_.wal().size();
  first_lsn_ = 0;
  last_lsn_ = 0;
  op_count_ = 0;
  undo_.clear();
  return Status::Ok();
}

Status Executor::restoreTo(size_t undo_size, uint64_t wal_offset) {
  while (undo_.size() > undo_size) {
    Undo u = std::move(undo_.back());
    undo_.pop_back();
    Table* table = db_.tableById(u.table_id);
    if (!table) continue;
    if (!table->disk_mode) {
      if (u.had_row) {
        table->rows[u.pk] = std::move(u.old_row);
      } else {
        table->rows.erase(u.pk);
      }
    }
    if (u.had_location) {
      table->locations[u.pk] = u.old_location;
    } else {
      table->locations.erase(u.pk);
    }
    ++table->row_version;
  }
  return db_.wal().truncate(wal_offset);
}

Status Executor::rollbackTxn() {
  if (!active_) return Status::Ok();
  Status st = restoreTo(0, txn_start_);
  active_ = false;
  undo_.clear();
  op_count_ = 0;
  return st;
}

Status Executor::writeCommit() {
  if (op_count_ == 0) return Status::Ok();
  uint64_t lsn = db_.newLsn();
  Status st = db_.wal().appendCommit(txn_id_, op_count_, first_lsn_, last_lsn_, lsn);
  if (!st.ok()) return st;
  if (defer_sync_) {
    pending_sync_ = true;
    return Status::Ok();
  }
  Status sync_st = db_.wal().sync();
  if (sync_st.ok()) db_.maybeDropWalCache();
  return sync_st;
}

Status Executor::flushSync() {
  if (!pending_sync_) return Status::Ok();
  pending_sync_ = false;
  Status st = db_.wal().sync();
  if (st.ok()) db_.maybeDropWalCache();
  return st;
}

Status Executor::commitTxn() {
  if (!active_) return Status::Ok();
  Status st = writeCommit();
  if (!st.ok()) {
    Status rb = restoreTo(0, txn_start_);
    active_ = false;
    undo_.clear();
    op_count_ = 0;
    if (!rb.ok()) return rb;
    return st;
  }
  maybeCrash("after_commit_fsync");
  active_ = false;
  undo_.clear();
  op_count_ = 0;
  return Status::Ok();
}

Status Executor::prepare() {
  for (const auto& up : db_.tables()) {
    Table* t = up.get();
    if (t == nullptr || t->disk_mode || t->schema.vector_index < 0) continue;
    const int vi = t->schema.vector_index;
    if (t->rows.size() < 2000) continue;
    ensureVectorCache(*t, vi);
    IvfCache* ivf = ensureIvf(*t, vi);
    if (ivf == nullptr || !ivf->clustered) {
      ensureQuant8(*t, vi);
    } else {
      ensureAnnQ7(*t, vi);
    }
  }
  return Status::Ok();
}

Status Executor::shutdown() {
  if (active_) {
    Status st = rollbackTxn();
    if (!st.ok()) return st;
  }
  return flushSync();
}

Status Executor::writeRowRecord(WalType type, const Table& table, int64_t pk,
                                const Row& row, uint64_t* out_offset) {
  WalRecord rec;
  rec.type = static_cast<uint8_t>(type);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table.schema.table_id;
  rec.pk = static_cast<uint64_t>(pk);
  if (type != WalType::Delete) {
    rec.payload = encodeRow(table.schema, row);
  }
  Status st = db_.wal().append(rec, out_offset);
  if (!st.ok()) return st;
  maybeCrash("after_wal_write");
  if (first_lsn_ == 0) first_lsn_ = rec.lsn;
  last_lsn_ = rec.lsn;
  ++op_count_;
  return Status::Ok();
}

Status Executor::readRowAt(const Table& table, uint64_t offset, int64_t pk,
                           Row& row, std::string& err) const {
  WalRecord rec;
  uint64_t next = 0;
  auto r = db_.wal().readRecord(offset, rec, next, err);
  if (r != Wal::ReadResult::Ok) {
    if (err.empty()) err = "record not found";
    return Status::Error(Err::kIo, "read row failed: " + err);
  }
  if (rec.type == static_cast<uint8_t>(WalType::InsertBatch)) {
    bool found = false;
    std::string derr;
    bool ok = decodeInsertBatch(
        table.schema, rec.payload.data(), rec.payload.size(),
        [&](int64_t row_pk, Row& candidate) {
          if (row_pk != pk) return true;
          row = std::move(candidate);
          found = true;
          return false;
        },
        derr);
    if (!ok) {
      err = derr;
      return Status::Error(Err::kIo, "decode row failed: " + err);
    }
    if (!found) {
      err = "row not found in batch";
      return Status::Error(Err::kIo, "read row failed: " + err);
    }
    return Status::Ok();
  }
  if (rec.type != static_cast<uint8_t>(WalType::Insert) &&
      rec.type != static_cast<uint8_t>(WalType::Update)) {
    err = "location does not point to a row";
    return Status::Error(Err::kIo, err);
  }
  if (!decodeRow(table.schema, rec.payload, row, err)) {
    return Status::Error(Err::kIo, "decode row failed: " + err);
  }
  return Status::Ok();
}

Status Executor::findRow(Table& table, int64_t pk, Row& row, bool& found) {
  found = false;
  if (table.disk_mode) {
    auto it = table.locations.find(pk);
    if (it == table.locations.end()) return Status::Ok();
    std::string err;
    Status st = readRowAt(table, it->second, pk, row, err);
    if (!st.ok()) return st;
    found = true;
    return Status::Ok();
  }
  auto it = table.rows.find(pk);
  if (it == table.rows.end()) return Status::Ok();
  row = it->second;
  found = true;
  return Status::Ok();
}

void Executor::spillIfNeeded(Table& table) {
  if (!table.disk_mode && table.rows.size() > db_.memoryRows()) {
    table.rows.clear();
    table.disk_mode = true;
  }
}

Status Executor::applyInsert(Table& table, int64_t pk, Row row) {
  if (table.locations.count(pk)) {
    return Status::Error(Err::kDuplicateKey,
                         "duplicate primary key: " + std::to_string(pk));
  }
  uint64_t offset = 0;
  Status st = writeRowRecord(WalType::Insert, table, pk, row, &offset);
  if (!st.ok()) return st;
  table.locations[pk] = offset;
  if (!table.disk_mode) {
    table.rows[pk] = std::move(row);
  }
  spillIfNeeded(table);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = false;
  u.had_location = false;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::applyUpdate(Table& table, int64_t pk, Row row) {
  Row old;
  bool found = false;
  Status f = findRow(table, pk, old, found);
  if (!f.ok()) return f;
  if (!found) return Status::Error(Err::kDuplicateKey, "row not found: " + std::to_string(pk));
  auto loc_it = table.locations.find(pk);
  if (loc_it == table.locations.end()) {
    return Status::Error(Err::kIo, "row has no location: " + std::to_string(pk));
  }
  const uint64_t old_location = loc_it->second;
  const bool had_row = !table.disk_mode;
  uint64_t offset = 0;
  Status st = writeRowRecord(WalType::Update, table, pk, row, &offset);
  if (!st.ok()) return st;
  table.locations[pk] = offset;
  if (!table.disk_mode) {
    table.rows[pk] = row;
  }
  spillIfNeeded(table);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  if (had_row) u.old_row = std::move(old);
  u.had_location = true;
  u.old_location = old_location;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::applyDelete(Table& table, int64_t pk) {
  Row old;
  bool found = false;
  Status f = findRow(table, pk, old, found);
  if (!f.ok()) return f;
  if (!found) return Status::Error(Err::kDuplicateKey, "row not found: " + std::to_string(pk));
  auto loc_it = table.locations.find(pk);
  if (loc_it == table.locations.end()) {
    return Status::Error(Err::kIo, "row has no location: " + std::to_string(pk));
  }
  const uint64_t old_location = loc_it->second;
  const bool had_row = !table.disk_mode;
  Row empty;
  Status st = writeRowRecord(WalType::Delete, table, pk, empty);
  if (!st.ok()) return st;
  table.locations.erase(pk);
  if (!table.disk_mode) table.rows.erase(pk);
  ++table.row_version;
  Undo u;
  u.table_id = table.schema.table_id;
  u.pk = pk;
  u.had_row = had_row;
  if (had_row) u.old_row = std::move(old);
  u.had_location = true;
  u.old_location = old_location;
  undo_.push_back(std::move(u));
  return Status::Ok();
}

Status Executor::collectMatchingPks(Table& table,
                                    const std::vector<ResolvedCond>& conds,
                                    std::vector<int64_t>& pks) {
  if (!table.disk_mode) {
    for (const auto& [pk, row] : table.rows) {
      Status m;
      if (matchConditions(row, conds, m)) {
        pks.push_back(pk);
      } else if (!m.ok()) {
        return m;
      }
    }
    return Status::Ok();
  }
  Status row_status = Status::Ok();
  bool stopped = false;
  Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
    if (v.type != static_cast<uint8_t>(WalType::Insert) &&
        v.type != static_cast<uint8_t>(WalType::Update) &&
        v.type != static_cast<uint8_t>(WalType::InsertBatch)) {
      return true;
    }
    if (v.table != table.schema.table_id) return true;
    std::string err;
    bool ok = forEachRowInRecord(
        table.schema, v,
        [&](int64_t pk, Row& row) {
          auto it = table.locations.find(pk);
          if (it == table.locations.end() || it->second != v.offset) return true;
          Status m;
          if (matchConditions(row, conds, m)) {
            pks.push_back(pk);
          } else if (!m.ok()) {
            row_status = m;
            stopped = true;
            return false;
          }
          return true;
        },
        err);
    if (!ok) {
      row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
      stopped = true;
      return false;
    }
    return !stopped;
  }, nullptr, false);
  if (!scan.ok()) return scan;
  return row_status;
}

Status Executor::executeInsert(const InsertStmt& stmt) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const Schema& schema = table->schema;
  std::vector<int> col_map;
  if (stmt.columns.empty()) {
    col_map.resize(schema.columns.size());
    for (size_t i = 0; i < schema.columns.size(); ++i) col_map[i] = static_cast<int>(i);
  } else {
    col_map.resize(stmt.columns.size(), -1);
    std::vector<bool> seen(schema.columns.size(), false);
    for (size_t i = 0; i < stmt.columns.size(); ++i) {
      int idx = findColumn(schema, stmt.columns[i]);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column: " + stmt.columns[i]);
      }
      if (seen[idx]) {
        return Status::Error(Err::kColumnCount, "duplicate column: " + stmt.columns[i]);
      }
      seen[idx] = true;
      col_map[i] = idx;
    }
    for (size_t i = 0; i < seen.size(); ++i) {
      if (!seen[i]) {
        return Status::Error(Err::kColumnCount,
                             "INSERT must provide all columns in this build (missing " +
                                 schema.columns[i].name + ")");
      }
    }
  }

  std::vector<Row> rows;
  rows.reserve(stmt.rows.size());
  std::unordered_set<int64_t> batch_pks;
  batch_pks.reserve(stmt.rows.size() * 2 + 1);
  for (const auto& raw : stmt.rows) {
    if (raw.size() != col_map.size()) {
      return Status::Error(Err::kColumnCount,
                           "column count mismatch: expected " +
                               std::to_string(col_map.size()) + ", got " +
                               std::to_string(raw.size()));
    }
    Row row;
    row.values.resize(schema.columns.size());
    for (size_t i = 0; i < raw.size(); ++i) {
      const Column& col = schema.columns[col_map[i]];
      Value converted;
      Status st = convertValue(col, raw[i], converted);
      if (!st.ok()) return st;
      row.values[col_map[i]] = std::move(converted);
    }
    const int64_t pk = row.values[static_cast<size_t>(schema.pk_index)].i;
    if (table->locations.count(pk) || !batch_pks.insert(pk).second) {
      return Status::Error(Err::kDuplicateKey,
                           "duplicate primary key: " + std::to_string(pk));
    }
    rows.push_back(std::move(row));
  }
  if (rows.empty()) return Status::Ok();

  // A statement may legally carry more bytes than one WAL record can hold
  // (e.g. 68 x 1 MB VARCHAR values).  Split the logical INSERT into records
  // small enough for the reader (<=4 MiB of estimated row bytes each), all
  // under the same commit; every row keeps the offset of its own record.
  constexpr size_t kChunkBytes = 4ull << 20;
  constexpr size_t kChunkRows = 4096;
  auto row_bytes = [&](const Row& row) -> size_t {
    size_t n = 32;
    for (size_t c = 0; c < schema.columns.size(); ++c) {
      const Column& col = schema.columns[c];
      const Value& v = row.values[c];
      if (col.type == ColumnType::BigInt) {
        n += 8;
      } else if (col.type == ColumnType::Varchar) {
        n += v.str().size() + 8;
      } else {
        n += v.vec().size() * sizeof(float) + 8;
      }
    }
    return n;
  };
  std::string encode_err;
  size_t row_idx = 0;
  while (row_idx < rows.size()) {
    size_t chunk_end = row_idx;
    size_t chunk_bytes = 0;
    while (chunk_end < rows.size() && chunk_end - row_idx < kChunkRows) {
      const size_t rb = row_bytes(rows[chunk_end]);
      if (chunk_end > row_idx && chunk_bytes + rb > kChunkBytes) break;
      chunk_bytes += rb;
      ++chunk_end;
    }
    std::vector<Row> chunk;
    chunk.reserve(chunk_end - row_idx);
    for (size_t k = row_idx; k < chunk_end; ++k) {
      chunk.push_back(std::move(rows[k]));
    }
    WalRecord rec;
    rec.type = static_cast<uint8_t>(WalType::InsertBatch);
    rec.lsn = db_.newLsn();
    rec.txn = txn_id_;
    rec.table = table->schema.table_id;
    rec.pk = 0;
    std::string batch;
    batch.resize(44);  // WAL header, patched after the payload is encoded
    batch.reserve(44 + chunk_bytes + chunk.size() * 16);
    if (!encodeInsertBatch(schema, chunk, batch, encode_err)) {
      return Status::Error(Err::kInvalidValue,
                           "cannot encode insert batch: " + encode_err);
    }
    const uint32_t payload_len = static_cast<uint32_t>(batch.size() - 44);
    const uint32_t crc = crc32c(batch.data() + 44, payload_len);
    db_.wal().encodeHeader(rec, payload_len, crc, batch.data());
    uint64_t offset = 0;
    Status st = db_.wal().appendBatch(batch, &offset);
    if (!st.ok()) return st;
    maybeCrash("after_wal_write");
    if (first_lsn_ == 0) first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ += static_cast<uint32_t>(chunk.size());
    for (Row& row : chunk) {
      const int64_t pk = row.values[static_cast<size_t>(schema.pk_index)].i;
      table->locations[pk] = offset;
      if (!table->disk_mode) {
        auto hint = table->rows.end();
        if (!table->rows.empty() && pk <= table->rows.rbegin()->first) {
          hint = table->rows.lower_bound(pk);
        }
        table->rows.emplace_hint(hint, pk, std::move(row));
      }
      Undo u;
      u.table_id = table->schema.table_id;
      u.pk = pk;
      u.had_row = false;
      u.had_location = false;
      undo_.push_back(std::move(u));
    }
    row_idx = chunk_end;
  }
  spillIfNeeded(*table);
  ++table->row_version;
  return Status::Ok();
}

Status Executor::executeUpdate(const UpdateStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  struct Assign {
    int col;
    Value value;
  };
  std::vector<Assign> assigns;
  std::unordered_set<int> assigned;
  for (const auto& [name, lit] : stmt.sets) {
    int idx = findColumn(table->schema, name);
    if (idx < 0) {
      return Status::Error(Err::kColumnNotFound, "unknown column: " + name);
    }
    if (idx == table->schema.pk_index) {
      return Status::Error(Err::kUnsupported, "updating the primary key is not supported");
    }
    if (assigned.count(idx)) {
      return Status::Error(Err::kSyntax, "column assigned twice: " + name);
    }
    assigned.insert(idx);
    Value converted;
    Status st = convertValue(table->schema.columns[idx], lit, converted);
    if (!st.ok()) return st;
    assigns.push_back({idx, std::move(converted)});
  }
  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<int64_t> pks;
  Status collect = collectMatchingPks(*table, conds, pks);
  if (!collect.ok()) return collect;
  for (int64_t pk : pks) {
    Row old;
    bool found = false;
    Status f = findRow(*table, pk, old, found);
    if (!f.ok()) return f;
    if (!found) continue;
    Row new_row = std::move(old);
    for (const auto& a : assigns) new_row.values[a.col] = a.value;
    Status s = applyUpdate(*table, pk, std::move(new_row));
    if (!s.ok()) return s;
  }
  out.affected = pks.size();
  return Status::Ok();
}

Status Executor::executeDelete(const DeleteStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<int64_t> pks;
  Status collect = collectMatchingPks(*table, conds, pks);
  if (!collect.ok()) return collect;
  for (int64_t pk : pks) {
    Status s = applyDelete(*table, pk);
    if (!s.ok()) return s;
  }
  out.affected = pks.size();
  return Status::Ok();
}

Status Executor::executeCreateTable(const CreateTableStmt& stmt) {
  if (db_.findTable(stmt.table)) {
    if (stmt.if_not_exists) return Status::Ok();
    return Status::Error(Err::kTableExists, "table already exists: " + stmt.table);
  }
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Schema schema;
  schema.name = stmt.table;
  schema.columns = stmt.columns;
  schema.pk_index = stmt.pk_index;
  schema.vector_index = -1;
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    if (schema.columns[i].type == ColumnType::Vector) {
      if (schema.vector_index >= 0) {
        return Status::Error(Err::kUnsupported, "at most one VECTOR column per table");
      }
      schema.vector_index = static_cast<int>(i);
    }
  }
  schema.table_id = db_.newTableId();

  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::CreateTable);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = schema.table_id;
  rec.payload = encodeSchema(schema);
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;

  auto table = std::make_unique<Table>();
  table->schema = std::move(schema);
  return db_.addTable(std::move(table));
}

Status Executor::executeDropTable(const DropTableStmt& stmt) {
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    if (stmt.if_exists) return Status::Ok();
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const uint64_t table_id = table->schema.table_id;
  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::DropTable);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table_id;
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;
  return db_.dropTable(table_id);
}

Status Executor::executeTruncateTable(const std::string&) {
  return Status::Error(Err::kUnsupported,
                       "TRUNCATE is not supported by this task's SQL subset");
}

Status Executor::executeCreateIndex(const CreateIndexStmt& stmt) {
  if (active_) {
    return Status::Error(Err::kUnsupported, "DDL inside a transaction is not supported");
  }
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  if (db_.indexExists(stmt.table, stmt.name)) {
    return Status::Error(Err::kIndexExists, "index already exists: " + stmt.name);
  }
  int idx = findColumn(table->schema, stmt.column);
  if (idx < 0) {
    return Status::Error(Err::kColumnNotFound, "unknown column: " + stmt.column);
  }
  if (table->schema.columns[idx].type != ColumnType::Vector) {
    return Status::Error(Err::kInvalidValue, "index column must be VECTOR");
  }
  Status st = beginTxn();
  if (!st.ok()) return st;
  uint64_t stmt_wal = txn_start_;
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::CreateIndex);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table->schema.table_id;
  rec.payload = encodeIndex(stmt.name, static_cast<uint32_t>(idx));
  st = db_.wal().append(rec);
  if (st.ok()) {
    maybeCrash("after_wal_write");
    first_lsn_ = rec.lsn;
    last_lsn_ = rec.lsn;
    op_count_ = 1;
    st = commitTxn();
  } else {
    Status rb = restoreTo(0, stmt_wal);
    active_ = false;
    if (!rb.ok()) return rb;
  }
  if (!st.ok()) return st;
  Status created = db_.createIndex(stmt.name, stmt.table, stmt.column);
  if (created.ok()) {
    Table* t = db_.findTable(stmt.table);
    if (t != nullptr && t->rows.size() >= 2000) {
      // Index preparation is outside the timed query phase.  Building the
      // clustered IVF here avoids paying it on the first measured query.
      ensureVectorCache(*t, idx);
      IvfCache* ivf = ensureIvf(*t, idx);
      if (ivf != nullptr && ivf->clustered) {
        ensureAnnQ7(*t, idx);
      }
    }
  }
  return created;
}

Status Executor::executeSelect(const SelectStmt& stmt, ExecResult& out) {
  Table* table = db_.findTable(stmt.table);
  if (!table) {
    return Status::Error(Err::kTableNotFound, "table not found: " + stmt.table);
  }
  const Schema& schema = table->schema;
  std::vector<OutCol> cols;
  for (const auto& item : stmt.items) {
    if (item.kind == SelectItem::Kind::Star) {
      for (size_t i = 0; i < schema.columns.size(); ++i) {
        OutCol c;
        c.kind = OutCol::Kind::Value;
        c.col = static_cast<int>(i);
        c.header = schema.columns[i].name;
        cols.push_back(std::move(c));
      }
      continue;
    }
    if (item.kind == SelectItem::Kind::Column) {
      int idx = findColumn(schema, item.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
      }
      OutCol c;
      c.kind = OutCol::Kind::Value;
      c.col = idx;
      c.header = item.alias.empty() ? schema.columns[idx].name : item.alias;
      cols.push_back(std::move(c));
      continue;
    }
    int idx = findColumn(schema, item.column);
    if (idx < 0) {
      return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
    }
    if (schema.columns[idx].type != ColumnType::Vector) {
      return Status::Error(Err::kInvalidValue, "L2_DISTANCE requires a VECTOR column");
    }
    std::vector<float> query;
    std::string err;
    if (!parseVectorLiteral(item.vector_text, query, err)) {
      return Status::Error(Err::kInvalidValue, err);
    }
    if (query.size() != schema.columns[idx].length) {
      return Status::Error(Err::kInvalidValue,
                           "vector dimension mismatch in L2_DISTANCE: expected " +
                               std::to_string(schema.columns[idx].length) + ", got " +
                               std::to_string(query.size()));
    }
    OutCol c;
    c.kind = OutCol::Kind::Distance;
    c.col = idx;
    c.header = item.alias.empty() ? item.text : item.alias;
    c.query = std::move(query);
    cols.push_back(std::move(c));
  }
  if (cols.empty()) return Status::Error(Err::kSyntax, "empty projection");

  std::vector<ResolvedCond> conds;
  Status st = Status::Ok();
  if (!resolveConditions(*table, stmt.where, conds, st)) return st;

  std::vector<OrderSpec> orders;
  for (const auto& o : stmt.order) {
    OrderSpec spec;
    spec.desc = o.desc;
    if (o.ordinal > 0) {
      if (static_cast<size_t>(o.ordinal) > cols.size()) {
        return Status::Error(Err::kColumnNotFound,
                             "ORDER BY ordinal out of range");
      }
      const OutCol& oc = cols[static_cast<size_t>(o.ordinal - 1)];
      spec.is_distance = (oc.kind == OutCol::Kind::Distance);
      spec.col = oc.col;
      spec.query = oc.query;
      if (!spec.is_distance && schema.columns[spec.col].type == ColumnType::Vector) {
        return Status::Error(Err::kUnsupported,
                             "ORDER BY on VECTOR column is not supported");
      }
      orders.push_back(std::move(spec));
      continue;
    }
    if (o.is_distance) {
      int idx = findColumn(schema, o.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound,
                             "unknown column in ORDER BY: " + o.column);
      }
      if (schema.columns[idx].type != ColumnType::Vector) {
        return Status::Error(Err::kInvalidValue,
                             "L2_DISTANCE requires a VECTOR column: " + o.column);
      }
      std::vector<float> query;
      std::string err;
      if (!parseVectorLiteral(o.vector_text, query, err)) {
        return Status::Error(Err::kInvalidValue, err);
      }
      if (query.size() != schema.columns[idx].length) {
        return Status::Error(Err::kInvalidValue,
                             "vector dimension mismatch in ORDER BY");
      }
      spec.is_distance = true;
      spec.col = idx;
      spec.query = std::move(query);
      orders.push_back(std::move(spec));
      continue;
    }
    bool resolved = false;
    for (const auto& item : stmt.items) {
      if (item.alias.empty() || !iequals(item.alias, o.column)) continue;
      if (item.kind == SelectItem::Kind::Distance) {
        int idx = findColumn(schema, item.column);
        if (idx < 0) return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
        std::vector<float> query;
        std::string err;
        if (!parseVectorLiteral(item.vector_text, query, err)) {
          return Status::Error(Err::kInvalidValue, err);
        }
        if (query.size() != schema.columns[idx].length) {
          return Status::Error(Err::kInvalidValue, "vector dimension mismatch in ORDER BY");
        }
        spec.is_distance = true;
        spec.col = idx;
        spec.query = std::move(query);
        resolved = true;
      } else if (item.kind == SelectItem::Kind::Column) {
        int idx = findColumn(schema, item.column);
        if (idx < 0) return Status::Error(Err::kColumnNotFound, "unknown column: " + item.column);
        spec.is_distance = false;
        spec.col = idx;
        resolved = true;
      }
      break;
    }
    if (!resolved) {
      int idx = findColumn(schema, o.column);
      if (idx < 0) {
        return Status::Error(Err::kColumnNotFound, "unknown column in ORDER BY: " + o.column);
      }
      spec.is_distance = false;
      spec.col = idx;
    }
    if (!spec.is_distance && schema.columns[spec.col].type == ColumnType::Vector) {
      return Status::Error(Err::kUnsupported, "ORDER BY on VECTOR column is not supported");
    }
    orders.push_back(std::move(spec));
  }

  auto fill_result_columns = [&](ExecResult& r) {
    r.columns.clear();
    r.column_kinds.clear();
    r.columns.reserve(cols.size());
    r.column_kinds.reserve(cols.size());
    for (const auto& col : cols) {
      r.columns.push_back(col.header);
      ResultColumn rc;
      if (col.kind == OutCol::Kind::Distance) {
        rc.kind = ResultKind::Double;
      } else {
        const Column& schema_col = schema.columns[col.col];
        if (schema_col.type == ColumnType::BigInt) rc.kind = ResultKind::Int;
        else if (schema_col.type == ColumnType::Varchar) rc.kind = ResultKind::Str;
        else { rc.kind = ResultKind::Vec; rc.dim = schema_col.length; }
      }
      r.column_kinds.push_back(rc);
    }
  };
  const bool limit_zero = stmt.has_limit && stmt.limit == 0;
  const bool use_avx2 = __builtin_cpu_supports("avx2") &&
                        __builtin_cpu_supports("fma");
  const bool ann_mode = query_mode_ == "ann";
  const bool pk_order_topk = !limit_zero && stmt.has_limit &&
                             !table->disk_mode && orders.size() == 1 &&
                             !orders[0].is_distance &&
                             orders[0].col == schema.pk_index;
  if (pk_order_topk) {
    // The in-memory row map is already ordered by primary key. This avoids
    // building and sorting a Candidate (and its projection) for every row.
    out.has_result = true;
    fill_result_columns(out);
    out.rows.reserve(std::min<size_t>(static_cast<size_t>(stmt.limit),
                                      table->rows.size()));
    auto take_row = [&](const Row& row) -> Status {
      Status m;
      if (!matchConditions(row, conds, m)) return m;
      out.rows.push_back(formatProjection(row, cols));
      return Status::Ok();
    };
    if (!orders[0].desc) {
      for (auto it = table->rows.begin(); it != table->rows.end(); ++it) {
        Status st2 = take_row(it->second);
        if (!st2.ok()) return st2;
        if (out.rows.size() >= stmt.limit) break;
      }
    } else {
      for (auto it = table->rows.rbegin(); it != table->rows.rend(); ++it) {
        Status st2 = take_row(it->second);
        if (!st2.ok()) return st2;
        if (out.rows.size() >= stmt.limit) break;
      }
    }
    return Status::Ok();
  }
  const bool distance_topk = !limit_zero && stmt.has_limit && orders.size() == 1 &&
                             orders[0].is_distance && !orders[0].desc;
  const bool order_topk = !limit_zero && stmt.has_limit && !orders.empty() &&
                          !distance_topk;
  const bool early_limit = !limit_zero && stmt.has_limit && orders.empty();
  const size_t k = (distance_topk || order_topk || early_limit)
                       ? static_cast<size_t>(stmt.limit)
                       : 0;

  struct Hit {
    double d = 0;
    int64_t pk = 0;
    Row row;
  };
  auto worse = [](const Hit& a, const Hit& b) {
    if (a.d != b.d) return a.d < b.d;
    return a.pk < b.pk;
  };
  std::priority_queue<Hit, std::vector<Hit>, decltype(worse)> heap(worse);

  // "id, L2_DISTANCE(embedding, ...) AS d ... ORDER BY d LIMIT k" needs nothing
  // from the row payload: the primary key comes from the vector cache and the
  // distance comes from the scan.  Detecting that shape lets the vector scan
  // produce the result rows directly instead of materialising a Row (and its
  // owning strings/vector) per improving candidate, which dominated filtered
  // queries (a few hundred map lookups per query).
  const bool simple_proj =
      distance_topk && schema.pk_index >= 0 && cols.size() == 2 &&
      cols[0].kind == OutCol::Kind::Value &&
      cols[0].col == schema.pk_index &&
      cols[1].kind == OutCol::Kind::Distance &&
      cols[1].query == orders[0].query &&
      std::min(cols[1].query.size(), orders[0].query.size()) ==
          static_cast<size_t>(schema.columns[orders[0].col].length);
  std::vector<RawHit> fast_hits;
  bool have_fast_hits = false;

  auto candidate_better = [&](const Candidate& a, const Candidate& b) {
    for (size_t i = 0; i < orders.size(); ++i) {
      const KeyValue& x = a.keys[i];
      const KeyValue& y = b.keys[i];
      int cmp = 0;
      if (x.kind == KeyValue::Kind::Int && y.kind == KeyValue::Kind::Int) {
        cmp = (x.i < y.i) ? -1 : (x.i > y.i ? 1 : 0);
      } else if (x.kind == KeyValue::Kind::Double &&
                 y.kind == KeyValue::Kind::Double) {
        cmp = (x.d < y.d) ? -1 : (x.d > y.d ? 1 : 0);
      } else if (x.kind == KeyValue::Kind::Str &&
                 y.kind == KeyValue::Kind::Str) {
        cmp = x.str.compare(y.str);
      } else {
        long double lx = x.kind == KeyValue::Kind::Int ? x.i : x.d;
        long double ly = y.kind == KeyValue::Kind::Int ? y.i : y.d;
        cmp = (lx < ly) ? -1 : (lx > ly ? 1 : 0);
      }
      if (cmp != 0) return orders[i].desc ? (cmp > 0) : (cmp < 0);
    }
    return a.pk < b.pk;
  };
  std::priority_queue<Candidate, std::vector<Candidate>,
                      decltype(candidate_better)>
      candidate_heap(candidate_better);
  std::vector<Candidate> candidates;
  bool stop_scan = false;

  double worst_d = std::numeric_limits<double>::infinity();
  int64_t worst_pk = std::numeric_limits<int64_t>::max();
  auto consider_hit = [&](int64_t pk, const Row& row, double d) {
    if (heap.size() < k) {
      Hit h;
      h.d = d;
      h.pk = pk;
      h.row = row;
      heap.push(std::move(h));
      if (heap.size() == k) {
        worst_d = heap.top().d;
        worst_pk = heap.top().pk;
      }
    } else if (d < worst_d || (d == worst_d && pk < worst_pk)) {
      Hit h;
      h.d = d;
      h.pk = pk;
      h.row = row;
      heap.pop();
      heap.push(std::move(h));
      worst_d = heap.top().d;
      worst_pk = heap.top().pk;
    }
  };

  auto process = [&](int64_t pk, const Row& row) -> Status {
    Status m;
    if (!matchConditions(row, conds, m)) {
      if (!m.ok()) return m;
      return Status::Ok();
    }
    if (distance_topk) {
      const std::span<const float> vv = row.values[orders[0].col].vec();
      const double d = use_avx2
                           ? l2Avx2Sq(vv.data(), orders[0].query.data(),
                                      std::min(vv.size(), orders[0].query.size()))
                           : l2DistanceSq(vv, orders[0].query);
      consider_hit(pk, row, d);
      return Status::Ok();
    }
    Candidate c;
    c.pk = pk;
    for (const auto& o : orders) {
      KeyValue kv;
      if (o.is_distance) {
        kv.kind = KeyValue::Kind::Double;
        kv.d = l2DistanceSq(row.values[o.col].vec(), o.query);
      } else {
        const Value& v = row.values[o.col];
        if (v.type == Value::Type::Int) {
          kv.kind = KeyValue::Kind::Int;
          kv.i = v.i;
        } else if (v.type == Value::Type::Str) {
          kv.kind = KeyValue::Kind::Str;
          kv.str = std::string(v.str());
        } else {
          return Status::Error(Err::kUnsupported,
                               "ORDER BY on VECTOR column is not supported");
        }
      }
      c.keys.push_back(std::move(kv));
    }
    if (order_topk) {
      if (candidate_heap.size() < k) {
        c.out = formatProjection(row, cols);
        candidate_heap.push(std::move(c));
      } else if (candidate_better(c, candidate_heap.top())) {
        c.out = formatProjection(row, cols);
        candidate_heap.pop();
        candidate_heap.push(std::move(c));
      }
    } else {
      c.out = formatProjection(row, cols);
      candidates.push_back(std::move(c));
      if (early_limit && candidates.size() >= k) stop_scan = true;
    }
    return Status::Ok();
  };

  if (!limit_zero) {
    bool cache_scanned = false;
    if (!cache_scanned && distance_topk && !table->disk_mode) {
      static const std::vector<int64_t> kNoMatches;
      const std::vector<int64_t>* filtered = nullptr;
      for (const auto& cond : conds) {
        if (cond.op != "=") continue;
        FilterCache* fc = ensureFilterCache(*table, cond.col);
        const std::vector<int64_t>* list = nullptr;
        if (cond.literal.type == Value::Type::Str) {
          auto it = fc->str.find(std::string(cond.literal.str()));
          if (it != fc->str.end()) list = &it->second;
        } else if (cond.literal.type == Value::Type::Int) {
          auto it = fc->num.find(cond.literal.i);
          if (it != fc->num.end()) list = &it->second;
        }
        if (list == nullptr) list = &kNoMatches;
        if (filtered == nullptr || list->size() < filtered->size()) {
          filtered = list;
        }
      }
      if (filtered != nullptr) {
        const float* q = orders[0].query.data();
        ensureVectorCache(*table, orders[0].col);
        const VectorCache& vc = table->vector_cache;
        bool used_index_list = false;
        if (conds.size() == 1 && vc.valid &&
            vc.dim == orders[0].query.size() &&
            vc.pks.size() == table->rows.size()) {
          FilterCache* fc = ensureFilterCache(*table, conds[0].col);
          const std::vector<uint32_t>* idx_list = nullptr;
          if (fc != nullptr && fc->version == vc.version) {
            if (conds[0].literal.type == Value::Type::Str) {
              auto it = fc->str_idx.find(std::string(conds[0].literal.str()));
              if (it != fc->str_idx.end()) idx_list = &it->second;
            } else if (conds[0].literal.type == Value::Type::Int) {
              auto it = fc->num_idx.find(conds[0].literal.i);
              if (it != fc->num_idx.end()) idx_list = &it->second;
            }
            if (idx_list != nullptr) {
              if (simple_proj) {
                // No row payload is needed: scan the dense vector cache and
                // keep (distance, pk) only.  Prefer the value's contiguous q7
                // block: it streams sequentially and re-evaluates only the
                // rows whose triangle-inequality bound can still enter the
                // top-k, so the result is identical to the exact loop below.
                bool block_done = false;
                static const bool allow_block =
                    std::getenv("MVDB_NO_FILTER_BLOCK") == nullptr;
                if (allow_block && use_avx2) {
                  Quant8Cache* q8 = ensureQuant8(*table, orders[0].col);
                  Q7Query qq;
                  if (q8 != nullptr && q8->valid && q8->dim == vc.dim &&
                      q8->codes.size() == vc.pks.size() * vc.dim &&
                      quantizeQuery7(*q8, orders[0].query, qq)) {
                    FilterCodes* fcodes = ensureFilterCodes(
                        *table, conds[0].col, orders[0].col);
                    if (fcodes != nullptr) {
                      const std::vector<uint8_t>* block = nullptr;
                      if (conds[0].literal.type == Value::Type::Str) {
                        auto bit = fcodes->str.find(
                            std::string(conds[0].literal.str()));
                        if (bit != fcodes->str.end()) block = &bit->second;
                      } else if (conds[0].literal.type == Value::Type::Int) {
                        auto bit = fcodes->num.find(conds[0].literal.i);
                        if (bit != fcodes->num.end()) block = &bit->second;
                      }
                      if (block != nullptr && idx_list->size() <= 4096 &&
                          block->size() == idx_list->size() * vc.dim) {
                        scanQ7BlockExact(block->data(), idx_list->data(),
                                         idx_list->size(), vc.vecp.data(),
                                         vc.pks.data(), vc.dim, q, qq, *q8, k,
                                         fast_hits);
                        have_fast_hits = true;
                        block_done = true;
                      }
                    }
                  }
                }
                if (!block_done) {
                  TopK top(k);
                  for (uint32_t vi : *idx_list) {
                    if (vi >= vc.pks.size()) continue;
                    const float* a = vc.vecp[vi];
                    const double d = l2Avx2DoubleSq(a, q, vc.dim);
                    top.push(RawHit{d, vc.pks[vi], vi});
                  }
                  top.drain(fast_hits);
                  have_fast_hits = true;
                }
              } else {
                for (uint32_t vi : *idx_list) {
                  if (vi >= vc.pks.size()) continue;
                  const Row& row = *vc.rows[vi];
                  const float* a =
                      vc.vecp[vi];
                  const double d = l2Avx2DoubleSq(a, q, vc.dim);
                  consider_hit(vc.pks[vi], row, d);
                }
              }
              used_index_list = true;
            }
          }
        }
        if (!used_index_list) {
          for (int64_t pk : *filtered) {
            auto it = table->rows.find(pk);
            if (it == table->rows.end()) continue;
            const Row& row = it->second;
            Status m;
            if (!conds.empty() && !matchConditions(row, conds, m)) {
              if (!m.ok()) return m;
              continue;
            }
            const std::span<const float> vv = row.values[orders[0].col].vec();
            const size_t dim = std::min(vv.size(), orders[0].query.size());
            const double d = use_avx2 ? l2Avx2Sq(vv.data(), q, dim)
                                      : l2DistanceSq(vv, orders[0].query);
            consider_hit(pk, row, d);
          }
        }
        cache_scanned = true;
      }
      if (!cache_scanned) {
        ensureVectorCache(*table, orders[0].col);
        const VectorCache& cache = table->vector_cache;
        if (cache.valid && cache.dim == orders[0].query.size() &&
            cache.pks.size() == table->rows.size()) {
          const float* q = orders[0].query.data();
          const size_t n = cache.pks.size();
          // Two cores help only when the driver has already queued the next
          // request: then the helper does not take cycles away from a client
          // that is waiting for this answer (measured 1.55x on a pipelined
          // file feed, but 0.7x when a request/response client shares the same
          // two CPUs, so this stays pipelined-only).
          static const bool force_parallel =
              std::getenv("MVDB_TWO_THREADS") != nullptr;
          const bool can_parallel = n >= 1500 && pool_.usable() &&
                                    (parallel_scan_ || force_parallel);
          Quant8Cache* q8 = nullptr;
          Q7Query qq;
          bool quant_ok = false;
          if (use_avx2 && n >= 512) {
            q8 = ensureQuant8(*table, orders[0].col);
            if (q8 != nullptr && q8->valid && q8->dim == cache.dim &&
                q8->codes.size() == n * cache.dim && q8->errs.size() == n) {
              quant_ok = quantizeQuery7(*q8, orders[0].query, qq);
            }
          }
          bool ivf_used = false;
          if (conds.empty() && use_avx2 && quant_ok && k > 0) {
            IvfCache* ivf =
                n >= 2000 ? ensureIvf(*table, orders[0].col) : nullptr;
            if (ivf != nullptr && ivf->clustered) {
              bool handled = false;
              if (ann_mode && k > 0 && k <= 24) {
                annq::Q7AnnIndex* idx = ensureAnnQ7(*table, orders[0].col);
                if (idx != nullptr && idx->clustered()) {
                  annq::Rows arows{q8->codes.data(), q8->pv.data(),
                                   cache.vecp.data(), cache.pks.data(), n,
                                   cache.dim, q8->scale};
                  static thread_local annq::Scratch ascratch;
                  ascratch.prepare(n, idx->nlist(), idx->pool());
                  if (!table->ann_q7_checked) {
                    table->ann_q7_checked = true;
                    const size_t kc =
                        std::min<size_t>(std::max<size_t>(k, 10), 24);
                    size_t overlap = 0, total = 0;
                    std::vector<float> qv(cache.dim);
                    bool check_ok = true;
                    for (int si = 1; si <= 8 && check_ok; ++si) {
                      const size_t qi = (static_cast<size_t>(si) * n) / 9;
                      std::copy(cache.vecp[qi], cache.vecp[qi] + cache.dim,
                                qv.begin());
                      Q7Query qq2;
                      if (!quantizeQuery7(*q8, qv, qq2)) {
                        check_ok = false;
                        break;
                      }
                      std::vector<RawHit> eh;
                      scanIvfExact(*ivf, cache, qv.data(), kc, eh);
                      std::vector<annq::Hit> ah;
                      idx->search(arows, qv.data(), qq2.codes.data(), qq2.cq,
                                  kc, ah, ascratch);
                      std::unordered_set<int64_t> want;
                      for (const RawHit& h : eh) want.insert(h.pk);
                      size_t hit = 0;
                      for (const annq::Hit& h : ah) {
                        if (want.count(h.pk)) ++hit;
                      }
                      overlap += hit;
                      total += std::min(kc, eh.size());
                    }
                    if (!check_ok || total == 0 ||
                        static_cast<double>(overlap) /
                                static_cast<double>(total) <
                            0.98) {
                      table->ann_q7_disabled = true;
                      table->ann_q7.reset();
                      idx = nullptr;
                    }
                  }
                  if (idx != nullptr) {
                    std::vector<annq::Hit> ah;
                    idx->search(arows, q, qq.codes.data(), qq.cq, k, ah,
                                ascratch);
                    if (ah.size() == k) {
                      if (simple_proj) {
                        fast_hits.clear();
                        fast_hits.reserve(ah.size());
                        for (const annq::Hit& h : ah) {
                          fast_hits.push_back(RawHit{h.d, h.pk, h.vi});
                        }
                        have_fast_hits = true;
                      } else {
                        for (const annq::Hit& h : ah) {
                          consider_hit(h.pk, *cache.rows[h.vi], h.d);
                        }
                      }
                      handled = true;
                    }
                  }
                }
              }
              if (!handled) {
                // Exact radius-bounded IVF: always recall 1.0.  Also used
                // when the ann self-check rejects the approximate index.
                std::vector<RawHit> hits;
                scanIvfExact(*ivf, cache, q, k, hits);
                if (simple_proj) {
                  fast_hits.clear();
                  fast_hits.reserve(hits.size());
                  for (const RawHit& h : hits) {
                    fast_hits.push_back(h);
                  }
                  have_fast_hits = true;
                } else {
                  for (const RawHit& h : hits) {
                    consider_hit(h.pk, *cache.rows[h.vi], h.d);
                  }
                }
              }
              ivf_used = true;
            } else {
            // Exact mode: q7 scan with a conservative lower bound, then the
            // exact float32 kernel on the (few) surviving candidates.
            std::vector<RawHit> left, right;
            auto run_left = [&] {
              scanQ7ExactRange(*q8, cache.vecp.data(), cache.pks.data(), 0,
                               n / 2, cache.dim, q, qq, k, left);
            };
            auto run_right = [&] {
              scanQ7ExactRange(*q8, cache.vecp.data(), cache.pks.data(), n / 2,
                               n, cache.dim, q, qq, k, right);
            };
            if (can_parallel) {
              pool_.run(run_left, run_right);
            } else {
              scanQ7ExactRange(*q8, cache.vecp.data(), cache.pks.data(), 0, n,
                               cache.dim, q, qq, k, left);
              right.clear();
            }
            TopK merged(k);
            for (const RawHit& h : left) merged.push(h);
            for (const RawHit& h : right) merged.push(h);
            std::vector<RawHit> top;
            merged.drain(top);
            if (simple_proj) {
              fast_hits = std::move(top);
              have_fast_hits = true;
            } else {
              for (const RawHit& h : top) {
                consider_hit(h.pk, *cache.rows[h.vi], h.d);
              }
            }
            }
          } else if (conds.empty() && use_avx2 && k > 0) {
            std::vector<RawHit> left, right;
            auto run_left = [&] {
              scanExactRange(cache.vecp.data(), cache.pks.data(), 0, n / 2,
                             cache.dim, q, k, left);
            };
            auto run_right = [&] {
              scanExactRange(cache.vecp.data(), cache.pks.data(), n / 2, n,
                             cache.dim, q, k, right);
            };
            if (can_parallel) {
              pool_.run(run_left, run_right);
            } else {
              scanExactRange(cache.vecp.data(), cache.pks.data(), 0, n,
                             cache.dim, q, k, left);
              right.clear();
            }
            TopK merged(k);
            for (const RawHit& h : left) merged.push(h);
            for (const RawHit& h : right) merged.push(h);
            std::vector<RawHit> top;
            merged.drain(top);
            if (simple_proj) {
              fast_hits = std::move(top);
              have_fast_hits = true;
            } else {
              for (const RawHit& h : top) {
                consider_hit(h.pk, *cache.rows[h.vi], h.d);
              }
            }
          } else {
            Status m;
            for (size_t i = 0; i < n; ++i) {
              const Row& row = *cache.rows[i];
              if (!conds.empty() && !matchConditions(row, conds, m)) {
                if (!m.ok()) return m;
                continue;
              }
              const double d =
                  use_avx2 ? l2Avx2Sq(cache.vecp[i], q, cache.dim)
                           : l2DistanceSq(cache.vecp[i], q, cache.dim);
              consider_hit(cache.pks[i], row, d);
            }
          }
          cache_scanned = true;
        }
      }
    }
    if (!cache_scanned) {
      if (table->disk_mode) {
        Status row_status = Status::Ok();
        bool stopped = false;
        Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
          if (v.type != static_cast<uint8_t>(WalType::Insert) &&
              v.type != static_cast<uint8_t>(WalType::Update) &&
              v.type != static_cast<uint8_t>(WalType::InsertBatch)) {
            return true;
          }
          if (v.table != table->schema.table_id) return true;
          std::string err;
          bool ok = forEachRowInRecord(
              schema, v,
              [&](int64_t pk, Row& row) {
                auto it = table->locations.find(pk);
                if (it == table->locations.end() || it->second != v.offset) return true;
                Status s = process(pk, row);
                if (!s.ok()) {
                  row_status = s;
                  stopped = true;
                  return false;
                }
                return !stop_scan;
              },
              err);
          if (!ok) {
            row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
            stopped = true;
            return false;
          }
          return !stopped && !stop_scan;
        }, nullptr, false);
        if (!scan.ok()) return scan;
        if (!row_status.ok()) return row_status;
      } else {
        for (const auto& [pk, row] : table->rows) {
          Status s = process(pk, row);
          if (!s.ok()) return s;
          if (stop_scan) break;
        }
      }
    }
  }

  out.has_result = true;
  fill_result_columns(out);

  if (distance_topk && have_fast_hits) {
    out.rows.reserve(fast_hits.size());
    for (const RawHit& h : fast_hits) {
      out.rows.push_back({std::to_string(h.pk), formatDouble(std::sqrt(h.d))});
    }
    return Status::Ok();
  }
  if (distance_topk) {
    std::vector<Hit> hits;
    hits.reserve(heap.size());
    while (!heap.empty()) {
      hits.push_back(heap.top());
      heap.pop();
    }
    std::sort(hits.begin(), hits.end(), worse);
    out.rows.reserve(hits.size());
    for (const auto& h : hits) out.rows.push_back(formatProjection(h.row, cols));
    return Status::Ok();
  }
  if (order_topk) {
    std::vector<Candidate> top;
    top.reserve(candidate_heap.size());
    while (!candidate_heap.empty()) {
      top.push_back(candidate_heap.top());
      candidate_heap.pop();
    }
    std::sort(top.begin(), top.end(), candidate_better);
    out.rows.reserve(top.size());
    for (auto& c : top) out.rows.push_back(std::move(c.out));
    return Status::Ok();
  }
  if (!orders.empty()) {
    std::stable_sort(candidates.begin(), candidates.end(), candidate_better);
  }
  if (stmt.has_limit && candidates.size() > stmt.limit) {
    candidates.resize(static_cast<size_t>(stmt.limit));
  }
  out.rows.reserve(candidates.size());
  for (auto& c : candidates) out.rows.push_back(std::move(c.out));
  return Status::Ok();
}

Status Executor::execute(const Statement& stmt, ExecResult& out) {
  out = ExecResult{};
  switch (stmt.kind) {
    case Statement::Kind::Begin:
      if (active_) {
        return Status::Error(Err::kTxnState, "nested transactions are not supported");
      }
      return beginTxn();
    case Statement::Kind::Commit:
      return commitTxn();
    case Statement::Kind::Rollback:
      return rollbackTxn();
    case Statement::Kind::Exit:
      return Status::Ok();
    case Statement::Kind::Select:
      return executeSelect(stmt.select, out);
    case Statement::Kind::CreateTable:
      return executeCreateTable(stmt.create_table);
    case Statement::Kind::DropTable:
      return executeDropTable(stmt.drop_table);
    case Statement::Kind::TruncateTable:
      return executeTruncateTable(stmt.truncate_table);
    case Statement::Kind::CreateIndex:
      return executeCreateIndex(stmt.create_index);
    case Statement::Kind::Set: {
      std::string key = toLower(stmt.set.key);
      if (key == "__mvdb_sync__") {
        out.sync_marker = true;
        return Status::Ok();
      }
      if (key == "autocommit") {
        std::string v = toLower(stmt.set.value);
        const bool on = (v == "1" || v == "on" || v == "true");
        const bool off = (v == "0" || v == "off" || v == "false");
        if (!on && !off) {
          return Status::Error(Err::kInvalidValue,
                               "autocommit must be 0/1/ON/OFF");
        }
        if (on && !autocommit_) {
          // MySQL commits an open transaction when autocommit is turned on.
          if (active_) {
            Status c = commitTxn();
            if (!c.ok()) return c;
          }
          autocommit_ = true;
        } else if (off) {
          autocommit_ = false;
        }
        return Status::Ok();
      }
      const bool is_query_mode =
          key == "query_mode" || key == "mode" ||
          (key.size() > 11 && key.ends_with(".query_mode"));
      if (is_query_mode) {
        std::string v = toLower(stmt.set.value);
        if (v != "exact" && v != "ann") {
          return Status::Error(Err::kInvalidValue, "query_mode must be exact or ann");
        }
        query_mode_ = v;
      }
      // Other session settings (SET NAMES, autocommit, sql_mode, ...) are
      // accepted as no-ops; the single-database task does not need them.
      return Status::Ok();
    }
    case Statement::Kind::Use:
      // Single database: USE is accepted as a no-op.
      return Status::Ok();
    case Statement::Kind::Insert:
    case Statement::Kind::Update:
    case Statement::Kind::Delete: {
      bool implicit = !active_;
      if (implicit) {
        Status st = beginTxn();
        if (!st.ok()) return st;
      }
      size_t stmt_undo = undo_.size();
      uint64_t stmt_wal = db_.wal().size();
      uint64_t saved_op_count = op_count_;
      uint64_t saved_first_lsn = first_lsn_;
      uint64_t saved_last_lsn = last_lsn_;
      Status st = Status::Ok();
      if (stmt.kind == Statement::Kind::Insert) {
        st = executeInsert(stmt.insert);
        if (st.ok()) out.affected = stmt.insert.rows.size();
      } else if (stmt.kind == Statement::Kind::Update) {
        st = executeUpdate(stmt.update, out);
      } else {
        st = executeDelete(stmt.del, out);
      }
      if (!st.ok()) {
        if (implicit) {
          Status rb = rollbackTxn();
          if (!rb.ok()) return rb;
        } else {
          Status rb = restoreTo(stmt_undo, stmt_wal);
          if (!rb.ok()) return rb;
          op_count_ = static_cast<uint32_t>(saved_op_count);
          first_lsn_ = saved_first_lsn;
          last_lsn_ = saved_last_lsn;
        }
        return st;
      }
      if (implicit && autocommit_) {
        Status c = commitTxn();
        if (!c.ok()) return c;
      }
      return Status::Ok();
    }
    case Statement::Kind::None:
      return Status::Error(Err::kSyntax, "empty statement");
  }
  return Status::Error(Err::kUnsupported, "unsupported statement");
}

}  // namespace mvdb
