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

#define USING_LOG_PREFIX STORAGE

#include <algorithm>
#include "common/object/ob_object.h"
#include "ob_text_daat_iter.h"

namespace oceanbase
{
namespace storage
{
int ObTextDaaTIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (nullptr != param.base_param_->relevance_expr_
             && OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  } else {
    mode_flag_ = param.mode_flag_;
    function_lookup_mode_ = param.function_lookup_mode_;
  }
  return ret;
}

void ObTextDaaTIter::reset()
{
  bm25_param_estimator_.reset();
  // this class can know the type of dim_iters_, but ObSRDaaTIterImpl maybe not
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRDaaTIterImpl::reset();
}

void ObTextDaaTIter::reuse(const bool switch_tablet)
{
  if ((OB_NOT_NULL(dim_iters_) && 0 == dim_iters_->count())) {
    // do nothing
  } else {
    bm25_param_estimator_.reuse(switch_tablet);
  }
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDaaTIter::pre_process()
{
  int ret = OB_SUCCESS;
  if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (iter_param_->need_project_relevance()) {
    if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
    }
  }
  return ret;
}

ObTextBMWIter::ObTextBMWIter()
  : ObSRBMWIterImpl(),
    bm25_param_estimator_() {}

void ObTextBMWIter::reuse(const bool switch_tablet)
{
  bm25_param_estimator_.reuse(switch_tablet);
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reuse();
    }
  }
  ObSRBMWIterImpl::reuse(switch_tablet);
}

void ObTextBMWIter::reset()
{
  bm25_param_estimator_.reset();
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i))->reset();
    }
  }
  ObSRBMWIterImpl::reset();
}

int ObTextBMWIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRBMWIterImpl::init(*param.base_param_, *param.dim_iters_,
                                           *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_FAIL(bm25_param_estimator_.init(param.bm25_param_est_ctx_))) {
  }
  return ret;
}

int ObTextBMWIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (dim_iters_->count() == 0) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(bm25_param_estimator_.do_estimation(*iter_param_->eval_ctx_))) {
  }

  if (FAILEDx(ObSRBMWIterImpl::get_next_rows(capacity, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObTextBMWIter::init_before_wand_process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!bm25_param_estimator_.is_estimated())) {
    ret = OB_ERR_UNEXPECTED;
  }

  for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
    ObTextRetrievalBlockMaxIter *block_max_iter = static_cast<ObTextRetrievalBlockMaxIter *>(dim_iters_->at(i));
    if (OB_FAIL(block_max_iter->init_block_max_iter(
        bm25_param_estimator_.get_total_doc_cnt(), bm25_param_estimator_.get_avg_doc_token_cnt()))) {
    }
  }
  return ret;
}

ObTextDocIdBitmapIter::ObTextDocIdBitmapIter()
  : ObSRDaaTIterImpl(),
    bitmap_(nullptr),
    bitmap_capacity_bits_(0),
    bitmap_used_bits_(0),
    emit_cursor_bits_(0),
    tablet_id_(0),
    tablet_id_inited_(false),
    built_(false),
    building_(false),
    overflow_sorted_(false),
    overflow_ids_(),
    overflow_emit_idx_(0),
    emit_buffer_(nullptr),
    emit_buffer_capacity_(0)
{
}

int ObTextDocIdBitmapIter::init(const ObTextDaaTParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.base_param_) || OB_ISNULL(param.dim_iters_) || OB_ISNULL(param.allocator_)
      || OB_ISNULL(param.relevance_collector_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObSRDaaTIterImpl::init(*param.base_param_, *param.dim_iters_,
                                            *param.allocator_, *param.relevance_collector_))) {
  } else if (OB_ISNULL(iter_param_->id_proj_expr_) || OB_ISNULL(iter_param_->eval_ctx_)
             || OB_ISNULL(iter_allocator_)) {
    ret = OB_INVALID_ARGUMENT;
  }
  return ret;
}

void ObTextDocIdBitmapIter::reset()
{
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reset();
    }
  }
  bitmap_ = nullptr;
  bitmap_capacity_bits_ = 0;
  bitmap_used_bits_ = 0;
  emit_cursor_bits_ = 0;
  tablet_id_ = 0;
  tablet_id_inited_ = false;
  built_ = false;
  building_ = false;
  overflow_sorted_ = false;
  overflow_ids_.reset();
  overflow_emit_idx_ = 0;
  emit_buffer_ = nullptr;
  emit_buffer_capacity_ = 0;
  ObSRDaaTIterImpl::reset();
}

void ObTextDocIdBitmapIter::reuse(const bool switch_tablet)
{
  if (OB_NOT_NULL(dim_iters_)) {
    for (int64_t i = 0; i < dim_iters_->count(); ++i) {
      static_cast<ObTextRetrievalDaaTTokenIter *>(dim_iters_->at(i))->reuse();
    }
  }
  // The bitmap buffer is allocated from the iterator arena, which outlives
  // reuse, so keep it and just clear the used part.
  if (OB_NOT_NULL(bitmap_) && bitmap_capacity_bits_ > 0) {
    MEMSET(bitmap_, 0, bitmap_capacity_bits_ / 8);
  }
  bitmap_used_bits_ = 0;
  emit_cursor_bits_ = 0;
  tablet_id_ = 0;
  tablet_id_inited_ = false;
  built_ = false;
  building_ = false;
  overflow_sorted_ = false;
  overflow_ids_.reuse();
  overflow_emit_idx_ = 0;
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDocIdBitmapIter::ensure_bitmap_capacity(const int64_t bit_cnt)
{
  int ret = OB_SUCCESS;
  if (bit_cnt <= bitmap_capacity_bits_) {
  } else {
    int64_t new_bits = MAX(bit_cnt, MAX(2 * bitmap_capacity_bits_, INIT_BITMAP_BITS));
    new_bits = (new_bits + 511) & ~((int64_t)511);
    if (OB_UNLIKELY(new_bits > MAX_BITMAP_BITS)) {
      new_bits = MAX_BITMAP_BITS;
    }
    void *buf = iter_allocator_->alloc(new_bits / 8);
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate doc id bitmap", K(ret), K(new_bits));
    } else {
      MEMSET(buf, 0, new_bits / 8);
      if (OB_NOT_NULL(bitmap_) && bitmap_capacity_bits_ > 0) {
        MEMCPY(buf, bitmap_, bitmap_capacity_bits_ / 8);
      }
      bitmap_ = static_cast<char *>(buf);
      bitmap_capacity_bits_ = new_bits;
    }
  }
  return ret;
}

int ObTextDocIdBitmapIter::add_doc_id_to_overflow(const common::ObDatum &id_datum)
{
  int ret = OB_SUCCESS;
  const ObString id_str = id_datum.get_string();
  void *buf = nullptr;
  if (OB_UNLIKELY(id_str.empty())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = iter_allocator_->alloc(id_str.length()))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, id_str.ptr(), id_str.length());
    const ObString copy(id_str.length(), static_cast<const char *>(buf));
    if (OB_FAIL(overflow_ids_.push_back(copy))) {
      LOG_WARN("failed to record document id", K(ret));
    } else {
      overflow_sorted_ = false;
    }
  }
  return ret;
}

int ObTextDocIdBitmapIter::add_doc_id(const common::ObDatum &id_datum)
{
  int ret = OB_SUCCESS;
  const ObString id_str = id_datum.get_string();
  if (OB_UNLIKELY(id_str.length() != OB_DOC_ID_COLUMN_BYTE_LENGTH)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("unexpected document id length", K(ret), K(id_str.length()));
  } else {
    const ObDocId *doc_id = reinterpret_cast<const ObDocId *>(id_str.ptr());
    const uint64_t seq_id = doc_id->seq_id_;
    if (!tablet_id_inited_) {
      tablet_id_ = doc_id->tablet_id_;
      tablet_id_inited_ = true;
    }
    if (OB_UNLIKELY(doc_id->tablet_id_ != tablet_id_)
        || OB_UNLIKELY(seq_id >= static_cast<uint64_t>(MAX_BITMAP_BITS))) {
      // A different tablet in the same stream cannot share the bitmap domain and
      // a document id beyond the bitmap bound has no bit to live in; both are
      // kept exact through the sorted side list.
      ret = add_doc_id_to_overflow(id_datum);
    } else if (OB_FAIL(ensure_bitmap_capacity(static_cast<int64_t>(seq_id) + 1))) {
    } else {
      bitmap_[seq_id >> 3] = static_cast<char>(bitmap_[seq_id >> 3] | (1 << (seq_id & 7)));
      if (static_cast<int64_t>(seq_id) + 1 > bitmap_used_bits_) {
        bitmap_used_bits_ = static_cast<int64_t>(seq_id) + 1;
      }
    }
  }
  return ret;
}

int ObTextDocIdBitmapIter::finish_overflow_set()
{
  int ret = OB_SUCCESS;
  if (!overflow_sorted_) {
    // Sort by the same binary order the merge heap uses, then drop duplicates.
    std::sort(overflow_ids_.begin(), overflow_ids_.end(),
              [](const ObString &l, const ObString &r) {
                const int64_t min_len = MIN(l.length(), r.length());
                const int cmp = (min_len > 0) ? MEMCMP(l.ptr(), r.ptr(), min_len) : 0;
                return (0 != cmp) ? (cmp < 0) : (l.length() < r.length());
              });
    int64_t unique_cnt = 0;
    for (int64_t i = 0; i < overflow_ids_.count(); ++i) {
      const ObString &cur = overflow_ids_.at(i);
      if (unique_cnt > 0) {
        const ObString &prev = overflow_ids_.at(unique_cnt - 1);
        if (prev.length() == cur.length()
            && 0 == MEMCMP(prev.ptr(), cur.ptr(), cur.length())) {
          continue;
        }
      }
      if (unique_cnt != i) {
        overflow_ids_.at(unique_cnt) = cur;
      }
      ++unique_cnt;
    }
    while (overflow_ids_.count() > unique_cnt) {
      overflow_ids_.pop_back();
    }
    overflow_sorted_ = true;
  }
  return ret;
}

int ObTextDocIdBitmapIter::build_doc_id_set()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(building_) || OB_UNLIKELY(built_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    building_ = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < dim_iters_->count(); ++i) {
      ObISRDaaTDimIter *dim_iter = dim_iters_->at(i);
      if (OB_ISNULL(dim_iter)) {
        ret = OB_ERR_UNEXPECTED;
      }
      while (OB_SUCC(ret)) {
        const ObDatum *id_datum = nullptr;
        int tmp_ret = dim_iter->get_next_row();
        if (OB_SUCCESS == tmp_ret) {
          if (OB_FAIL(dim_iter->get_curr_id(id_datum))) {
          } else if (OB_ISNULL(id_datum)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(add_doc_id(*id_datum))) {
          }
        } else if (OB_ITER_END == tmp_ret) {
          break;
        } else {
          ret = tmp_ret;
          LOG_WARN("failed to read inverted index row", K(ret), K(i));
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(finish_overflow_set())) {
    }
    building_ = false;
  }
  return ret;
}

int ObTextDocIdBitmapIter::emit_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObExpr *id_proj_expr = iter_param_->id_proj_expr_;
  const int64_t real_capacity = MIN(MIN(capacity, iter_param_->max_batch_size_),
                                    eval_ctx->max_batch_size_);
  if (OB_UNLIKELY(real_capacity <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(real_capacity > emit_buffer_capacity_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("emit buffer too small", K(ret), K(real_capacity), K(emit_buffer_capacity_));
  } else {
    ObDatum *id_datums = id_proj_expr->locate_datums_for_update(*eval_ctx, real_capacity);
    sql::ObBitVector &id_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    guard.set_batch_size(real_capacity);
    const int64_t word_cnt = (bitmap_used_bits_ + 63) >> 6;
    const uint64_t *words = OB_NOT_NULL(bitmap_)
        ? reinterpret_cast<const uint64_t *>(bitmap_) : nullptr;
    while (OB_SUCC(ret) && count < real_capacity) {
      int64_t bit = -1;
      if (emit_cursor_bits_ >= 0) {
        int64_t word_idx = emit_cursor_bits_ >> 6;
        uint64_t word = 0;
        if (word_idx < word_cnt && OB_NOT_NULL(words)) {
          word = words[word_idx] & (~static_cast<uint64_t>(0) << (emit_cursor_bits_ & 63));
        }
        while (0 == word && ++word_idx < word_cnt) {
          word = words[word_idx];
        }
        if (0 != word) {
          bit = (word_idx << 6) + __builtin_ctzll(word);
        }
      }
      char *buf = emit_buffer_ + count * OB_DOC_ID_COLUMN_BYTE_LENGTH;
      if (bit >= 0) {
        const ObDocId doc_id(tablet_id_, static_cast<uint64_t>(bit));
        MEMCPY(buf, &doc_id, OB_DOC_ID_COLUMN_BYTE_LENGTH);
        emit_cursor_bits_ = bit + 1;
      } else {
        emit_cursor_bits_ = -1;
        if (overflow_emit_idx_ >= overflow_ids_.count()) {
          ret = OB_ITER_END;
          break;
        }
        const ObString &overflow_id = overflow_ids_.at(overflow_emit_idx_);
        MEMCPY(buf, overflow_id.ptr(), OB_DOC_ID_COLUMN_BYTE_LENGTH);
        ++overflow_emit_idx_;
      }
      guard.set_batch_idx(count);
      id_datums[count].set_string(buf, static_cast<uint32_t>(OB_DOC_ID_COLUMN_BYTE_LENGTH));
      id_flags.set(count);
      id_proj_expr->set_evaluated_projected(*eval_ctx);
      ++count;
      ++output_row_cnt_;
      ++input_row_cnt_;
    }
  }
  return ret;
}

int ObTextDocIdBitmapIter::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (0 == dim_iters_->count()) {
    ret = OB_ITER_END;
  } else if (OB_UNLIKELY(built_ && -1 == emit_cursor_bits_
             && overflow_emit_idx_ >= overflow_ids_.count())) {
    ret = OB_ITER_END;
  } else {
    if (!built_) {
      if (OB_SUCC(ret) && OB_ISNULL(emit_buffer_)) {
        const int64_t capacity_rows = MAX(iter_param_->max_batch_size_, 1);
        void *buf = iter_allocator_->alloc(capacity_rows * OB_DOC_ID_COLUMN_BYTE_LENGTH);
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          emit_buffer_ = static_cast<char *>(buf);
          emit_buffer_capacity_ = capacity_rows;
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(build_doc_id_set())) {
      } else {
        built_ = true;
        emit_cursor_bits_ = 0;
        overflow_emit_idx_ = 0;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(emit_rows(capacity, count))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

