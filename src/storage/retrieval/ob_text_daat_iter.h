/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef OB_TEXT_DAAT_ITER_H_
#define OB_TEXT_DAAT_ITER_H_

#include "query/ob_sql_define.h"
#include "ob_inv_idx_param_estimator.h"
#include "ob_sparse_bmw_iter.h"
#include "ob_sparse_daat_iter.h"
#include "ob_text_retrieval_token_iter.h"

namespace oceanbase
{
namespace sql
{
  class ObDASScanIter;
}
namespace storage
{

struct ObTextDaaTParam
{
  ObTextDaaTParam()
    : dim_iters_(nullptr), base_param_(nullptr), allocator_(nullptr), relevance_collector_(nullptr),
      bm25_param_est_ctx_(), mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false) {}
  ~ObTextDaaTParam() {}
  TO_STRING_KV(K_(base_param), KP_(dim_iters), K_(bm25_param_est_ctx),  K_(mode_flag), K_(function_lookup_mode));
  ObIArray<ObISRDaaTDimIter *> *dim_iters_;
  ObSparseRetrievalMergeParam *base_param_;
  common::ObArenaAllocator *allocator_;
  ObSRDaaTRelevanceCollector *relevance_collector_;
  ObBM25ParamEstCtx bm25_param_est_ctx_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
};

class ObTextDaaTIter final : public ObSRDaaTIterImpl
{
public:
  ObTextDaaTIter() : ObSRDaaTIterImpl(),
      bm25_param_estimator_(),
      mode_flag_(sql::ObMatchAgainstMode::NATURAL_LANGUAGE_MODE),
      function_lookup_mode_(false) {}
  virtual ~ObTextDaaTIter() { reset(); }

  int init(const ObTextDaaTParam &param);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
protected:
  virtual int pre_process() override;
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
  sql::ObMatchAgainstMode mode_flag_;
  bool function_lookup_mode_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextDaaTIter);
};


class ObTextBMWIter final : public ObSRBMWIterImpl
{
public:
  ObTextBMWIter();
  virtual ~ObTextBMWIter() { reset(); }
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  int init(const ObTextDaaTParam &param);
protected:
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  virtual int init_before_wand_process() override;
protected:
  ObBM25ParamEstimator bm25_param_estimator_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextBMWIter);
};

// Deduplicating iterator for retrieval that only needs the *set* of matching
// documents and no relevance score.
//
// In that shape the merge only exists to remove documents that match several
// query tokens, and the whole k-way merge (heap pushes, relevance collection and
// per-posting comparisons) is paid for a bit per document.  Instead, this
// iterator ORs the postings of every dimension into a document id bitmap and
// then walks the bitmap once, which turns the per-posting work into a single
// bit set and makes the per-document cost disappear.
//
// Only valid when every consumer of the scan is order insensitive, which is the
// case the optimizer's no-score gate selects: no projected column, no LIMIT, no
// ORDER BY / DISTINCT / GROUP BY / window function and no score filter.
class ObTextDocIdBitmapIter final : public ObSRDaaTIterImpl
{
public:
  ObTextDocIdBitmapIter();
  virtual ~ObTextDocIdBitmapIter() { reset(); }
  int init(const ObTextDaaTParam &param);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
private:
  int build_doc_id_set();
  int add_doc_id(const common::ObDatum &id_datum);
  int add_doc_id_to_overflow(const common::ObDatum &id_datum);
  int ensure_bitmap_capacity(const int64_t bit_cnt);
  int finish_overflow_set();
  int emit_rows(const int64_t capacity, int64_t &count);
private:
  // Upper bound of the bitmap: 64M documents per tablet (8MB).  Documents
  // beyond it, or any document from a different tablet in the same stream, are
  // kept in a sorted side list instead so the result stays exact.
  static const int64_t MAX_BITMAP_BYTES = 8L * 1024 * 1024;
  static const int64_t MAX_BITMAP_BITS = MAX_BITMAP_BYTES * 8;
  static const int64_t INIT_BITMAP_BITS = 64L * 1024;
private:
  char *bitmap_;
  int64_t bitmap_capacity_bits_;
  int64_t bitmap_used_bits_;
  int64_t emit_cursor_bits_;
  uint64_t tablet_id_;
  bool tablet_id_inited_;
  bool built_;
  bool building_;
  bool overflow_sorted_;
  common::ObSEArray<common::ObString, 8> overflow_ids_;
  int64_t overflow_emit_idx_;
  char *emit_buffer_;
  int64_t emit_buffer_capacity_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextDocIdBitmapIter);
};

} // namespace storage
} // namespace oceanbase

#endif
