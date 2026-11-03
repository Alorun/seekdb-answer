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

public:
  // Bitmap of the document ids seen for one tablet, allocated in fixed size
  // blocks so that growing never copies a previous buffer and the memory used is
  // exactly ceil(max_seq_id + 1, BLOCK_BITS) blocks.
  struct TabletBits
  {
    TabletBits() : tablet_id_(0), blocks_(), used_bits_(0) {}
    uint64_t tablet_id_;
    common::ObSEArray<char *, 8> blocks_;
    int64_t used_bits_;
  };
  static const int64_t BLOCK_BITS = 64 * 1024;              // 8 KB per block
public:
  static const int64_t MAX_BLOCKS_PER_TABLET = 1024;        // 8 MB per tablet
  static const int64_t MAX_TABLET_CNT = 8;
  static const int64_t MAX_TOTAL_BLOCKS = 2048;             // 16 MB per query

private:
  int build_doc_id_set();
  int add_doc_id(const common::ObDatum &id_datum);
  int get_tablet_bits(const uint64_t tablet_id, TabletBits *&tablet_bits);
  int ensure_block(TabletBits &tablet_bits, const int64_t block_idx);
  int emit_rows(const int64_t capacity, int64_t &count);
  int emit_rows_count_only(const int64_t capacity, int64_t &count);
private:
  common::ObSEArray<TabletBits, 4> tablet_bits_;
  int64_t total_block_cnt_;
  int64_t emit_tablet_idx_;
  int64_t emit_cursor_bits_;
  bool built_;
  bool building_;
  char *emit_buffer_;
  int64_t emit_buffer_capacity_;
private:
  DISALLOW_COPY_AND_ASSIGN(ObTextDocIdBitmapIter);
};

} // namespace storage
} // namespace oceanbase

#endif
