#include "ann_q7.h"

#include <immintrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace mvdb::annq {
namespace {

// ---------------------------------------------------------------------------
// Kernels.  These mirror, byte for byte, the ones in src/executor.cpp so that
// ann-mode distances and orderings match exact mode:
//   * q7 row codes are stored biased (b = code + 63 in [0,126]) and the query
//     codes stay signed in [-63,63];  sum((code_row - code_query)^2) is then
//     P_row + C_query - 2 * dot(b_row, s_query) in exact int32 arithmetic;
//   * exact distances accumulate in double.
// If the executor kernels change, change these too (they are 40 lines).
// ---------------------------------------------------------------------------
__attribute__((target("avx2"), always_inline)) inline int32_t hsum256(
    __m256i acc) {
  __m128i lo = _mm256_castsi256_si128(acc);
  __m128i hi = _mm256_extracti128_si256(acc, 1);
  __m128i sum = _mm_add_epi32(lo, hi);
  sum = _mm_hadd_epi32(sum, sum);
  sum = _mm_hadd_epi32(sum, sum);
  return _mm_cvtsi128_si32(sum);
}

__attribute__((target("avx2"), always_inline)) inline int32_t q7Dot(
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
  int32_t dot = hsum256(acc);
  for (; i < n; ++i) {
    dot += static_cast<int32_t>(a[i]) * static_cast<int32_t>(b[i]);
  }
  return dot;
}

// Four rows share the query-code loads; this is the hot loop of the flat scan.
__attribute__((target("avx2"), always_inline)) inline void q7Dot4(
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

__attribute__((target("avx2,fma"))) inline double l2ExactSq(const float* a,
                                                            const float* b,
                                                            size_t n) {
  __m256d acc0 = _mm256_setzero_pd();
  __m256d acc1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256d a0 = _mm256_cvtps_pd(_mm256_castps256_ps128(va));
    __m256d a1 = _mm256_cvtps_pd(_mm256_extractf128_ps(va, 1));
    __m256d b0 = _mm256_cvtps_pd(_mm256_castps256_ps128(vb));
    __m256d b1 = _mm256_cvtps_pd(_mm256_extractf128_ps(vb, 1));
    __m256d d0 = _mm256_sub_pd(a0, b0);
    __m256d d1 = _mm256_sub_pd(a1, b1);
    acc0 = _mm256_fmadd_pd(d0, d0, acc0);
    acc1 = _mm256_fmadd_pd(d1, d1, acc1);
  }
  alignas(32) double t0[4], t1[4];
  _mm256_store_pd(t0, acc0);
  _mm256_store_pd(t1, acc1);
  double sum = t0[0] + t0[1] + t0[2] + t0[3] + t1[0] + t1[1] + t1[2] + t1[3];
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return sum;
}

__attribute__((target("avx2,fma"))) inline float l2FloatSq(const float* a,
                                                           const float* b,
                                                           size_t n) {
  __m256 acc0 = _mm256_setzero_ps();
  __m256 acc1 = _mm256_setzero_ps();
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    __m256 d0 = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    __m256 d1 =
        _mm256_sub_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8));
    acc0 = _mm256_fmadd_ps(d0, d0, acc0);
    acc1 = _mm256_fmadd_ps(d1, d1, acc1);
  }
  for (; i + 8 <= n; i += 8) {
    __m256 d = _mm256_sub_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i));
    acc0 = _mm256_fmadd_ps(d, d, acc0);
  }
  __m256 acc = _mm256_add_ps(acc0, acc1);
  alignas(32) float t[8];
  _mm256_store_ps(t, acc);
  float s = ((t[0] + t[1]) + (t[2] + t[3])) + ((t[4] + t[5]) + (t[6] + t[7]));
  for (; i < n; ++i) {
    float d = a[i] - b[i];
    s += d * d;
  }
  return s;
}

// ---------------------------------------------------------------------------
// Candidate pool: append-only with periodic compaction.  The scan hot path is
// one compare and (rarely) two stores per row.
// ---------------------------------------------------------------------------
class Pool {
 public:
  Pool(int32_t* cv, uint32_t* ci, size_t cap, size_t pool, const int64_t* pks)
      : cv_(cv), ci_(ci), cap_(cap), pool_(pool), pks_(pks) {}

  // Sorted-insert into a small fixed array: no allocation, no compaction pass,
  // and the threshold is always the current worst pool member.
  inline void add(int32_t v, uint32_t i) {
    if (n_ < pool_) {
      if (n_ > 0 && v >= cv_[n_ - 1]) {
        cv_[n_] = v;
        ci_[n_] = i;
        ++n_;
        return;
      }
      size_t p = n_;
      while (p > 0 && cv_[p - 1] > v) {
        cv_[p] = cv_[p - 1];
        ci_[p] = ci_[p - 1];
        --p;
      }
      cv_[p] = v;
      ci_[p] = i;
      ++n_;
      return;
    }
    if (v >= cv_[n_ - 1]) return;
    size_t p = n_ - 1;
    while (p > 0 && cv_[p - 1] > v) {
      cv_[p] = cv_[p - 1];
      ci_[p] = ci_[p - 1];
      --p;
    }
    cv_[p] = v;
    ci_[p] = i;
  }

  void seed(int32_t v, uint32_t i) { add(v, i); }
  size_t size() const { return n_; }
  const int32_t* v() const { return cv_; }
  const uint32_t* i() const { return ci_; }
  // The pool is already the `pool_` smallest code distances; the exact
  // re-rank re-sorts by exact distance and pk, so no final ordering is needed.
  void finish() {}

 private:
  int32_t* cv_;
  uint32_t* ci_;
  size_t cap_, pool_;
  const int64_t* pks_;
  size_t n_ = 0;
};

}  // namespace

void Q7AnnIndex::clear() {
  n_ = dim_ = 0;
  clustered_ = false;
  nlist_ = nprobe_ = 0;
  cent_.clear();
  ccode_.clear();
  cpv_.clear();
  order_.clear();
  off_.clear();
}

size_t Q7AnnIndex::bytes() const {
  return cent_.size() * sizeof(float) + ccode_.size() + cpv_.size() * 4 +
         order_.size() * 4 + off_.size() * 4;
}

__attribute__((target("avx2,fma"))) void Q7AnnIndex::build(const Rows& rows, const Options& opt) {
  clear();
  opt_ = opt;
  n_ = rows.n;
  dim_ = rows.dim;
  clustered_ = false;
  if (n_ == 0 || dim_ == 0 || rows.codes == nullptr) return;
  if (n_ < opt_.ivf_min_rows || opt_.nlist == 0 || opt_.nprobe == 0) return;
  nlist_ = std::max<size_t>(4, std::min(opt_.nlist, n_ / 4));
  nprobe_ = std::max<size_t>(1, std::min(opt_.nprobe, nlist_));

  const size_t dim = dim_;
  const size_t n = n_;
  const int iters = std::max(1, opt_.kmeans_iters);
  cent_.assign(nlist_ * dim, 0.0f);
  // Deterministic spread init from the caller's row order.
  for (size_t c = 0; c < nlist_; ++c)
    std::memcpy(cent_.data() + c * dim, rows.vecp[(c * n) / nlist_], dim * 4);

  std::vector<uint32_t> assign(n, 0);
  std::vector<double> sum(nlist_ * dim, 0.0);
  std::vector<size_t> cnt(nlist_, 0);

  double total_var = 0.0;
  {
    std::vector<double> mean(dim, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const float* v = rows.vecp[i];
      for (size_t j = 0; j < dim; ++j) mean[j] += v[j];
    }
    for (size_t j = 0; j < dim; ++j) mean[j] /= static_cast<double>(n);
    for (size_t i = 0; i < n; ++i) {
      const float* v = rows.vecp[i];
      double d2 = 0.0;
      for (size_t j = 0; j < dim; ++j) {
        double df = static_cast<double>(v[j]) - mean[j];
        d2 += df * df;
      }
      total_var += d2;
    }
  }

  double inertia = 0.0;
  for (int it = 0; it < iters; ++it) {
    std::fill(sum.begin(), sum.end(), 0.0);
    std::fill(cnt.begin(), cnt.end(), 0);
    inertia = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const float* v = rows.vecp[i];
      size_t best = 0;
      float bd = std::numeric_limits<float>::max();
      for (size_t c = 0; c < nlist_; ++c) {
        float d = l2FloatSq(v, cent_.data() + c * dim, dim);
        if (d < bd) {
          bd = d;
          best = c;
        }
      }
      assign[i] = static_cast<uint32_t>(best);
      inertia += bd;
      cnt[best]++;
      double* s = sum.data() + best * dim;
      for (size_t j = 0; j < dim; ++j) s[j] += v[j];
    }
    for (size_t c = 0; c < nlist_; ++c) {
      if (cnt[c] == 0) continue;
      float* cc = cent_.data() + c * dim;
      const double* s = sum.data() + c * dim;
      const double inv = 1.0 / static_cast<double>(cnt[c]);
      for (size_t j = 0; j < dim; ++j)
        cc[j] = static_cast<float>(s[j] * inv);
    }
  }
  // Final assignment with the converged centroids (the loop's last assignment
  // used the previous centroids, so recompute once).
  inertia = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const float* v = rows.vecp[i];
    size_t best = 0;
    float bd = std::numeric_limits<float>::max();
    for (size_t c = 0; c < nlist_; ++c) {
      float d = l2FloatSq(v, cent_.data() + c * dim, dim);
      if (d < bd) {
        bd = d;
        best = c;
      }
    }
    assign[i] = static_cast<uint32_t>(best);
    inertia += bd;
  }
  const double ratio = total_var > 0.0 ? inertia / total_var : 1.0;
  if (!(ratio < opt_.ivf_max_inertia_ratio)) {
    // Not clusterable: the flat code scan is the safe path.  Keep no IVF
    // state at all (0 extra bytes).
    cent_.clear();
    cent_.shrink_to_fit();
    nlist_ = nprobe_ = 0;
    return;
  }
  clustered_ = true;

  off_.assign(nlist_ + 1, 0);
  for (size_t i = 0; i < n; ++i) off_[assign[i] + 1]++;
  for (size_t c = 0; c < nlist_; ++c) off_[c + 1] += off_[c];
  order_.resize(n);
  std::vector<uint32_t> cursor(off_.begin(), off_.end() - 1);
  for (size_t i = 0; i < n; ++i)
    order_[cursor[assign[i]]++] = static_cast<uint32_t>(i);

  // Centroid codes in the same q7 space as the rows.
  ccode_.resize(nlist_ * dim);
  cpv_.resize(nlist_);
  const int32_t bias_const = static_cast<int32_t>(dim) * 63 * 63;
  const float inv_scale =
      rows.scale > 0.0f && std::isfinite(rows.scale) ? 1.0f / rows.scale : 1.0f;
  for (size_t c = 0; c < nlist_; ++c) {
    int64_t sb = 0, sb2 = 0;
    for (size_t j = 0; j < dim; ++j) {
      long qv = std::lrint(cent_[c * dim + j] * inv_scale);
      qv = std::max(-63L, std::min(63L, qv));
      const int b = static_cast<int>(qv) + 63;
      ccode_[c * dim + j] = static_cast<uint8_t>(b);
      sb += b;
      sb2 += static_cast<int64_t>(b) * b;
    }
    cpv_[c] = static_cast<int32_t>(sb2 - 126 * sb + bias_const);
  }
}

__attribute__((target("avx2,fma"))) void Q7AnnIndex::search(
    const Rows& rows, const float* q, const int8_t* qcodes, int32_t cq,
    size_t k, std::vector<Hit>& out, Scratch& sc) const {
  out.clear();
  if (n_ == 0 || k == 0 || rows.n != n_) return;
  const size_t dim = dim_;
  const size_t pool = std::max<size_t>(k, std::min(opt_.pool, size_t{60}));
  const size_t cap = pool * 4 + 64;
  if (sc.cand_v.size() < cap) {
    sc.cand_v.resize(cap);
    sc.cand_i.resize(cap);
  }
  Pool pl(sc.cand_v.data(), sc.cand_i.data(), cap, pool, rows.pks);
  const uint8_t* codes = rows.codes;
  const int32_t* pv = rows.pv;

  if (clustered_) {
    // Coarse ranking on centroid codes (nlist * dim bytes, L2-resident).
    static thread_local std::vector<std::pair<int32_t, uint32_t>> tmp;
    tmp.clear();
    tmp.reserve(nlist_);
    for (size_t c = 0; c < nlist_; ++c) {
      const int32_t dd =
          cpv_[c] + cq - 2 * q7Dot(ccode_.data() + c * dim, qcodes, dim);
      tmp.push_back({dd, static_cast<uint32_t>(c)});
    }
    const size_t np = std::min(nprobe_, nlist_);
    std::partial_sort(tmp.begin(), tmp.begin() + np, tmp.end());
    for (size_t p = 0; p < np; ++p) {
      const uint32_t c = tmp[p].second;
      size_t b = off_[c], e = off_[c + 1];
      size_t i = b;
      for (; i + 4 <= e; i += 4) {
        const uint32_t* r4 = order_.data() + i;
        int32_t dq[4];
        q7Dot4(codes + r4[0] * dim, codes + r4[1] * dim, codes + r4[2] * dim,
               codes + r4[3] * dim, qcodes, dim, dq);
        pl.seed(pv[r4[0]] + cq - 2 * dq[0], r4[0]);
        pl.seed(pv[r4[1]] + cq - 2 * dq[1], r4[1]);
        pl.seed(pv[r4[2]] + cq - 2 * dq[2], r4[2]);
        pl.seed(pv[r4[3]] + cq - 2 * dq[3], r4[3]);
      }
      for (; i < e; ++i) {
        const uint32_t vi = order_[i];
        pl.seed(pv[vi] + cq - 2 * q7Dot(codes + vi * dim, qcodes, dim), vi);
      }
    }
  } else {
    const size_t n = n_;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
      int32_t dq[4];
      q7Dot4(codes + i * dim, codes + (i + 1) * dim, codes + (i + 2) * dim,
             codes + (i + 3) * dim, qcodes, dim, dq);
      pl.seed(pv[i] + cq - 2 * dq[0], static_cast<uint32_t>(i));
      pl.seed(pv[i + 1] + cq - 2 * dq[1], static_cast<uint32_t>(i + 1));
      pl.seed(pv[i + 2] + cq - 2 * dq[2], static_cast<uint32_t>(i + 2));
      pl.seed(pv[i + 3] + cq - 2 * dq[3], static_cast<uint32_t>(i + 3));
    }
    for (; i < n; ++i)
      pl.seed(pv[i] + cq - 2 * q7Dot(codes + i * dim, qcodes, dim),
              static_cast<uint32_t>(i));
  }
  pl.finish();

  // Exact float32 re-rank of the pool.  Prefetch every row first: the rows are
  // scattered over the float matrix, so serial evaluation would pay one DRAM
  // latency per row.
  const size_t m = pl.size();
  for (size_t t = 0; t < m; ++t) {
    const float* p = rows.vecp[pl.i()[t]];
    for (size_t b = 0; b < dim * 4; b += 64)
      _mm_prefetch(reinterpret_cast<const char*>(p) + b, _MM_HINT_T0);
  }
  struct Cand {
    double d;
    int64_t pk;
    size_t vi;
  };
  static thread_local std::vector<Cand> cd;
  cd.clear();
  for (size_t t = 0; t < m; ++t) {
    const size_t vi = pl.i()[t];
    cd.push_back({l2ExactSq(rows.vecp[vi], q, dim), rows.pks[vi], vi});
  }
  const size_t kk = std::min(k, cd.size());
  std::partial_sort(cd.begin(), cd.begin() + kk, cd.end(),
                    [](const Cand& a, const Cand& b) {
                      if (a.d != b.d) return a.d < b.d;
                      return a.pk < b.pk;
                    });
  out.reserve(kk);
  for (size_t t = 0; t < kk; ++t) out.push_back(Hit{cd[t].d, cd[t].pk, cd[t].vi});
}

Q7AnnIndex* ensureIndex(void*& slot, const Rows& rows, const Options& opt,
                        BuildStats* stats) {
  Q7AnnIndex* idx = static_cast<Q7AnnIndex*>(slot);
  if (idx == nullptr) {
    idx = new Q7AnnIndex();
    slot = idx;
  }
  const auto t0 = std::chrono::steady_clock::now();
  idx->build(rows, opt);
  if (stats != nullptr) {
    stats->ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    stats->bytes = idx->bytes();
    stats->clustered = idx->clustered();
    stats->nlist = idx->nlist();
    stats->nprobe = idx->nprobe();
  }
  return idx;
}

}  // namespace mvdb::annq
