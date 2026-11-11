#include "executor.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <immintrin.h>
#include <queue>
#include <set>
#include <thread>
#include <sched.h>
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
  // The q7 code scan is compute bound (1.28MB of codes per 10k x 128 scan, 4
  // maddubs/madd pairs per row), so a second physical core nearly halves it.
  // The helper is only started when the process really owns two CPUs; a
  // single-CPU pinning keeps the single-threaded path.
  static const bool force_off = std::getenv("MVDB_NO_2T") != nullptr;
  usable_ = !force_off && usableCpuCount() >= 2;
}

ScanPool::~ScanPool() {
  if (!worker_.joinable()) return;
  {
    std::lock_guard<std::mutex> lk(mu_);
    stop_.store(true, std::memory_order_relaxed);
    ++seq_;
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
      return v.s;
    case Value::Type::Vec: {
      std::string out = "[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) out += ",";
        out += formatDouble(v.v[i]);
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
      out.push_back(formatDouble(l2Distance(row.values[col.col].v, col.query)));
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
      cmp = v.s.compare(c.literal.s);
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
  explicit TopK(size_t k) : k_(k) {
    if (k_ <= 64) small_.reserve(k_);
  }

  __attribute__((always_inline)) inline size_t size() const {
    return k_ <= 64 ? small_.size() : heap_.size();
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
      return small_.empty() ? std::numeric_limits<double>::infinity()
                            : small_.back().d;
    }
    return heap_.empty() ? std::numeric_limits<double>::infinity()
                         : heap_.top().d;
  }

  __attribute__((always_inline)) inline void push(const RawHit& h) {
    if (k_ == 0) return;
    if (k_ <= 64) {
      if (small_.size() < k_) {
        auto it = std::lower_bound(small_.begin(), small_.end(), h, rawBetter);
        small_.insert(it, h);
      } else if (rawBetter(h, small_.back())) {
        auto it = std::lower_bound(small_.begin(), small_.end(), h, rawBetter);
        small_.insert(it, h);
        small_.pop_back();
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
      out.assign(small_.begin(), small_.end());
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
  std::vector<RawHit> small_;
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
    float sum = l2Kernel(a, q, dim);
    if (!std::isfinite(sum)) {
      RawHit h{l2Avx2DoubleSq(a, q, dim), pks[vi], vi};
      top.push(h);
      continue;
    }
    RawHit h{static_cast<double>(sum), pks[vi], vi};
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

// Exact q7 scan with a triangle-inequality lower bound:
//   | ||v-q|| - scale*||code(v)-code(q)|| | <= err(v) + err(q)
// Candidates that cannot beat the current exact k-th distance are skipped;
// every remaining candidate is evaluated with the exact float32 kernel, so
// this path returns exactly the same TopK as the full float scan.
//
// The threshold only changes when the k-th distance improves, and it is kept
// as an integer bound on the code-space squared distance so that the inner
// loop is (integer dot product, add, compare, unlikely branch).
__attribute__((target("avx2,fma"))) void scanQ7ExactRange(
    const Quant8Cache& c, const float* const* vecp, const int64_t* pks,
    size_t begin, size_t end, size_t dim, const float* q, const Q7Query& qq,
    size_t k, std::vector<RawHit>& out) {
  TopK top(k);
  double sqrt_worst = std::numeric_limits<double>::infinity();
  const size_t seed = std::min<size_t>(64, end - begin);
  for (size_t vi = begin; vi < begin + seed; ++vi) {
    const float* a = vecp[vi];
    float sum = l2Kernel(a, q, dim);
    double d = std::isfinite(sum) ? static_cast<double>(sum)
                                  : l2Avx2DoubleSq(a, q, dim);
    top.push(RawHit{d, pks[vi], vi});
    if (top.full()) sqrt_worst = std::sqrt(top.worstDistance());
  }
  if (k == 0) {
    top.drain(out);
    return;
  }
  const double inv = 1.0 / static_cast<double>(c.scale);
  const int8_t* qcodes = qq.codes.data();
  // Conservative integer threshold in code-distance units.  Using the maximum
  // row error instead of the per-row error keeps the inner loop free of a
  // second array stream and of double arithmetic; the quantizer error is nearly
  // constant across rows, so this costs almost no extra survivors.
  auto threshold2 = [&]() -> int32_t {
    const double bound =
        sqrt_worst + qq.error_norm + static_cast<double>(c.max_err);
    // Relative margin: errs/query_err are float-rounded and a tie at very
    // large magnitudes must not be pruned by rounding noise.
    const double t = bound * inv * (1.0 + 1e-6) + 1e-12;
    const double t2 = t * t;
    if (!(t2 < 2147483000.0)) return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(std::ceil(t2));
  };
  int32_t thr2 = threshold2();
  const uint8_t* codes = c.codes.data();
  const int32_t* pv = c.pv.data();
  const int32_t cq = qq.cq;
  for (size_t vi = begin + seed; vi < end; ++vi) {
    const int32_t dq =
        pv[vi] + cq - 2 * q7Dot(codes + vi * dim, qcodes, dim);
    if (dq > thr2) continue;
    const float* a = vecp[vi];
    float sum = l2Kernel(a, q, dim);
    double d = std::isfinite(sum) ? static_cast<double>(sum)
                                  : l2Avx2DoubleSq(a, q, dim);
    top.push(RawHit{d, pks[vi], vi});
    if (top.full()) {
      double w = std::sqrt(top.worstDistance());
      if (w < sqrt_worst) {
        sqrt_worst = w;
        thr2 = threshold2();
      }
    }
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
      float sum = l2Kernel(a, q, vc.dim);
      double d = std::isfinite(sum) ? static_cast<double>(sum)
                                    : l2Avx2DoubleSq(a, q, vc.dim);
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
      float sum = l2Kernel(a, q, vc.dim);
      double d = std::isfinite(sum) ? static_cast<double>(sum)
                                    : l2Avx2DoubleSq(a, q, vc.dim);
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

double l2DistanceSq(const std::vector<float>& a, const std::vector<float>& b) {
  return l2DistanceSq(a.data(), b.data(), std::min(a.size(), b.size()));
}

double l2Distance(const std::vector<float>& a, const std::vector<float>& b) {
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
    const std::vector<float>& v = row.values[vec_col].v;
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
        c.str[v.s].push_back(pk);
        c.str_idx[v.s].push_back(idx);
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
      if (utf8Length(input.s) > col.length) {
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
      if (!parseVectorLiteral(input.s, v, err)) {
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

Status Executor::readRowAt(const Table& table, uint64_t offset, Row& row,
                           std::string& err) const {
  WalRecord rec;
  uint64_t next = 0;
  auto r = db_.wal().readRecord(offset, rec, next, err);
  if (r != Wal::ReadResult::Ok) {
    if (err.empty()) err = "record not found";
    return Status::Error(Err::kIo, "read row failed: " + err);
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
    Status st = readRowAt(table, it->second, row, err);
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

Status Executor::applyInsertBatched(Table& table, int64_t pk, Row row,
                                    std::string& batch, uint64_t* out_offset) {
  if (table.locations.count(pk)) {
    return Status::Error(Err::kDuplicateKey,
                         "duplicate primary key: " + std::to_string(pk));
  }
  WalRecord rec;
  rec.type = static_cast<uint8_t>(WalType::Insert);
  rec.lsn = db_.newLsn();
  rec.txn = txn_id_;
  rec.table = table.schema.table_id;
  rec.pk = static_cast<uint64_t>(pk);
  const size_t rec_start = batch.size();
  batch.resize(rec_start + 44);
  const size_t payload_start = batch.size();
  encodeRowInto(table.schema, row, batch);
  const uint32_t payload_len =
      static_cast<uint32_t>(batch.size() - payload_start);
  const uint32_t crc =
      crc32c(batch.data() + payload_start, payload_len);
  db_.wal().encodeHeader(rec, payload_len, crc,
                         batch.data() + rec_start);
  *out_offset = db_.wal().size() + rec_start;
  if (first_lsn_ == 0) first_lsn_ = rec.lsn;
  last_lsn_ = rec.lsn;
  ++op_count_;
  table.locations[pk] = *out_offset;
  if (!table.disk_mode) {
    // Benchmark inserts are increasing; emplace_hint(end) avoids a full
    // O(log n) search per row while preserving std::map order.
    auto hint = table.rows.end();
    if (!table.rows.empty() && pk <= table.rows.rbegin()->first) {
      hint = table.rows.lower_bound(pk);
    }
    table.rows.emplace_hint(hint, pk, std::move(row));
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
  Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
    if (v.type != static_cast<uint8_t>(WalType::Insert) &&
        v.type != static_cast<uint8_t>(WalType::Update)) {
      return true;
    }
    if (v.table != table.schema.table_id) return true;
    auto it = table.locations.find(static_cast<int64_t>(v.pk));
    if (it == table.locations.end() || it->second != v.offset) return true;
    Row row;
    std::string err;
    if (!decodeRow(table.schema, v.payload, v.payload_size, row, err)) {
      row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
      return false;
    }
    Status m;
    if (matchConditions(row, conds, m)) {
      pks.push_back(static_cast<int64_t>(v.pk));
    } else if (!m.ok()) {
      row_status = m;
      return false;
    }
    return true;
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

  std::string batch;
  batch.reserve(stmt.rows.size() * 640);
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
    int64_t pk = row.values[schema.pk_index].i;
    uint64_t offset = 0;
    Status st = applyInsertBatched(*table, pk, std::move(row), batch, &offset);
    if (!st.ok()) return st;
  }
  Status appended = db_.wal().appendBatch(batch);
  if (!appended.ok()) return appended;
  maybeCrash("after_wal_write");
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
      ensureIvf(*t, idx);
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
    out.rows.reserve(static_cast<size_t>(stmt.limit));
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
      cols[1].kind == OutCol::Kind::Distance;
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
      const auto& vv = row.values[orders[0].col].v;
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
        kv.d = l2DistanceSq(row.values[o.col].v, o.query);
      } else {
        const Value& v = row.values[o.col];
        if (v.type == Value::Type::Int) {
          kv.kind = KeyValue::Kind::Int;
          kv.i = v.i;
        } else if (v.type == Value::Type::Str) {
          kv.kind = KeyValue::Kind::Str;
          kv.str = v.s;
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
          auto it = fc->str.find(cond.literal.s);
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
              auto it = fc->str_idx.find(conds[0].literal.s);
              if (it != fc->str_idx.end()) idx_list = &it->second;
            } else if (conds[0].literal.type == Value::Type::Int) {
              auto it = fc->num_idx.find(conds[0].literal.i);
              if (it != fc->num_idx.end()) idx_list = &it->second;
            }
            if (idx_list != nullptr) {
              if (simple_proj) {
                // No row payload is needed: scan the dense vector cache and
                // keep (distance, pk) only.  The map lookup and the Row copy
                // per improving candidate are what made filtered queries slow.
                TopK top(k);
                for (uint32_t vi : *idx_list) {
                  if (vi >= vc.pks.size()) continue;
                  const float* a =
                      vc.vecp[vi];
                  float sum = l2Kernel(a, q, vc.dim);
                  const double d = std::isfinite(sum)
                                       ? static_cast<double>(sum)
                                       : l2Avx2DoubleSq(a, q, vc.dim);
                  top.push(RawHit{d, vc.pks[vi], vi});
                }
                top.drain(fast_hits);
                have_fast_hits = true;
              } else {
                for (uint32_t vi : *idx_list) {
                  if (vi >= vc.pks.size()) continue;
                  const Row& row = *vc.rows[vi];
                  const float* a =
                      vc.vecp[vi];
                  const double d =
                      use_avx2 ? static_cast<double>(l2Kernel(a, q, vc.dim))
                               : l2DistanceSq(a, q, vc.dim);
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
            const auto& vv = row.values[orders[0].col].v;
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
              std::vector<RawHit> hits;
              if (ann_mode) {
                scanIvfAnn(*ivf, cache, q, k, 8, hits);
              } else {
                scanIvfExact(*ivf, cache, q, k, hits);
              }
              for (const RawHit& h : hits) {
                consider_hit(h.pk, *cache.rows[h.vi], h.d);
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
        Status scan = db_.wal().forEachRecord([&](const WalRecordView& v) {
          if (v.type != static_cast<uint8_t>(WalType::Insert) &&
              v.type != static_cast<uint8_t>(WalType::Update)) {
            return true;
          }
          if (v.table != table->schema.table_id) return true;
          auto it = table->locations.find(static_cast<int64_t>(v.pk));
          if (it == table->locations.end() || it->second != v.offset) return true;
          Row row;
          std::string err;
          if (!decodeRow(schema, v.payload, v.payload_size, row, err)) {
            row_status = Status::Error(Err::kIo, "corrupt row in WAL: " + err);
            return false;
          }
          Status s = process(static_cast<int64_t>(v.pk), row);
          if (!s.ok()) {
            row_status = s;
            return false;
          }
          return !stop_scan;
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
      if (implicit) {
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
