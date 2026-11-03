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
    tablet_bits_(),
    total_block_cnt_(0),
    emit_tablet_idx_(0),
    emit_cursor_bits_(0),
    built_(false),
    building_(false),
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
  } else if (OB_FAIL(tablet_bits_.reserve(4))) {
    LOG_WARN("failed to reserve tablet bitmap array", K(ret));
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
  tablet_bits_.reset();
  total_block_cnt_ = 0;
  emit_tablet_idx_ = 0;
  emit_cursor_bits_ = 0;
  built_ = false;
  building_ = false;
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
  // The bitmap blocks are allocated from the iterator arena, which outlives
  // reuse, so keep them and just clear the used part.
  for (int64_t i = 0; i < tablet_bits_.count(); ++i) {
    TabletBits &bits = tablet_bits_.at(i);
    bits.used_bits_ = 0;
    for (int64_t b = 0; b < bits.blocks_.count(); ++b) {
      MEMSET(bits.blocks_.at(b), 0, BLOCK_BITS / 8);
    }
  }
  emit_tablet_idx_ = 0;
  emit_cursor_bits_ = 0;
  built_ = false;
  building_ = false;
  ObSRDaaTIterImpl::reuse(switch_tablet);
}

int ObTextDocIdBitmapIter::get_tablet_bits(const uint64_t tablet_id, TabletBits *&tablet_bits)
{
  int ret = OB_SUCCESS;
  tablet_bits = nullptr;
  for (int64_t i = 0; i < tablet_bits_.count(); ++i) {
    if (tablet_bits_.at(i).tablet_id_ == tablet_id) {
      tablet_bits = &tablet_bits_.at(i);
      break;
    }
  }
  if (OB_ISNULL(tablet_bits)) {
    TabletBits fresh;
    fresh.tablet_id_ = tablet_id;
    if (tablet_bits_.count() >= MAX_TABLET_CNT) {
      // A single retrieval task is bound to one tablet; more than a handful of
      // distinct tablets means the stream is not the shape this iterator is
      // meant for.  Refuse instead of accumulating one bitmap per tablet.
      ret = OB_SIZE_OVERFLOW;
      LOG_WARN("too many tablets in one document id bitmap stream", K(ret), K(tablet_id),
               K(tablet_bits_.count()));
    } else if (OB_FAIL(tablet_bits_.push_back(fresh))) {
      LOG_WARN("failed to record tablet bitmap", K(ret));
    } else {
      tablet_bits = &tablet_bits_.at(tablet_bits_.count() - 1);
    }
  }
  return ret;
}

int ObTextDocIdBitmapIter::ensure_block(TabletBits &tablet_bits, const int64_t block_idx)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(block_idx < 0 || block_idx >= MAX_BLOCKS_PER_TABLET)) {
    ret = OB_SIZE_OVERFLOW;
    LOG_WARN("document id beyond the bitmap bound", K(ret), K(block_idx),
             K(tablet_bits.tablet_id_));
  } else {
    while (OB_SUCC(ret) && tablet_bits.blocks_.count() <= block_idx) {
      void *buf = nullptr;
      if (OB_UNLIKELY(total_block_cnt_ >= MAX_TOTAL_BLOCKS)) {
        ret = OB_SIZE_OVERFLOW;
        LOG_WARN("document id bitmap budget exhausted", K(ret), K(total_block_cnt_));
      } else if (OB_ISNULL(buf = iter_allocator_->alloc(BLOCK_BITS / 8))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        LOG_WARN("failed to allocate document id bitmap block", K(ret));
      } else if (OB_FAIL(tablet_bits.blocks_.push_back(static_cast<char *>(buf)))) {
        LOG_WARN("failed to record bitmap block", K(ret));
      } else {
        MEMSET(buf, 0, BLOCK_BITS / 8);
        ++total_block_cnt_;
      }
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
    TabletBits *tablet_bits = nullptr;
    if (OB_FAIL(get_tablet_bits(doc_id->tablet_id_, tablet_bits))) {
    } else if (OB_ISNULL(tablet_bits)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const int64_t block_idx = static_cast<int64_t>(seq_id) / BLOCK_BITS;
      if (OB_FAIL(ensure_block(*tablet_bits, block_idx))) {
      } else {
        char *block = tablet_bits->blocks_.at(block_idx);
        const int64_t in_block = static_cast<int64_t>(seq_id) % BLOCK_BITS;
        block[in_block >> 3] = static_cast<char>(block[in_block >> 3] | (1 << (in_block & 7)));
        if (static_cast<int64_t>(seq_id) + 1 > tablet_bits->used_bits_) {
          tablet_bits->used_bits_ = static_cast<int64_t>(seq_id) + 1;
        }
      }
    }
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
    building_ = false;
  }
  return ret;
}

namespace {
// The bit at a global word index of one tablet's block based bitmap.
inline uint64_t bitmap_word_at(const ObTextDocIdBitmapIter::TabletBits &tablet_bits,
                               const int64_t word_idx)
{
  const int64_t words_per_block = ObTextDocIdBitmapIter::BLOCK_BITS / 64;
  const int64_t block_idx = word_idx / words_per_block;
  const int64_t word_in_block = word_idx % words_per_block;
  return reinterpret_cast<const uint64_t *>(tablet_bits.blocks_.at(block_idx))[word_in_block];
}
} // namespace

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
  } else if (iter_param_->count_only_) {
    // When the scan output is only a row count, the enclosing operator counts
    // the returned rows and no expression ever reads the document id datum.
    ret = emit_rows_count_only(real_capacity, count);
  } else if (OB_UNLIKELY(real_capacity > emit_buffer_capacity_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("emit buffer too small", K(ret), K(real_capacity), K(emit_buffer_capacity_));
  } else {
    ObDatum *id_datums = id_proj_expr->locate_datums_for_update(*eval_ctx, real_capacity);
    sql::ObBitVector &id_flags = id_proj_expr->get_evaluated_flags(*eval_ctx);
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    guard.set_batch_size(real_capacity);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (emit_tablet_idx_ >= tablet_bits_.count()) {
        ret = OB_ITER_END;
        break;
      }
      const TabletBits &tablet_bits = tablet_bits_.at(emit_tablet_idx_);
      const int64_t words_per_block = BLOCK_BITS / 64;
      const int64_t word_cnt = tablet_bits.blocks_.count() * words_per_block;
      int64_t word_idx = emit_cursor_bits_ >> 6;
      uint64_t word = 0;
      if (word_idx < word_cnt) {
        word = bitmap_word_at(tablet_bits, word_idx)
            & (~static_cast<uint64_t>(0) << (emit_cursor_bits_ & 63));
      }
      while (0 == word && ++word_idx < word_cnt) {
        word = bitmap_word_at(tablet_bits, word_idx);
      }
      if (0 == word) {
        // this tablet is exhausted, continue with the next one
        ++emit_tablet_idx_;
        emit_cursor_bits_ = 0;
        continue;
      }
      const int64_t bit = (word_idx << 6) + __builtin_ctzll(word);
      const ObDocId doc_id(tablet_bits.tablet_id_, static_cast<uint64_t>(bit));
      char *buf = emit_buffer_ + count * OB_DOC_ID_COLUMN_BYTE_LENGTH;
      MEMCPY(buf, &doc_id, OB_DOC_ID_COLUMN_BYTE_LENGTH);
      emit_cursor_bits_ = bit + 1;
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

// Counting-only emit: the whole stream is consumed as a row count, so the
// document ids never leave this iterator and neither the emit buffer nor the
// projected datum is touched.  The cursor still advances by exactly the rows
// that are counted: whole words are consumed with a popcount, and only a batch
// boundary inside a word needs the set bits to be located one by one.
int ObTextDocIdBitmapIter::emit_rows_count_only(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  count = 0;
  const int64_t words_per_block = BLOCK_BITS / 64;
  while (OB_SUCC(ret) && count < capacity) {
    if (emit_tablet_idx_ >= tablet_bits_.count()) {
      ret = OB_ITER_END;
      break;
    }
    const TabletBits &tablet_bits = tablet_bits_.at(emit_tablet_idx_);
    const int64_t word_cnt = tablet_bits.blocks_.count() * words_per_block;
    int64_t word_idx = emit_cursor_bits_ >> 6;
    if (word_idx >= word_cnt) {
      // this tablet is exhausted, continue with the next one
      ++emit_tablet_idx_;
      emit_cursor_bits_ = 0;
      continue;
    }
    uint64_t word = bitmap_word_at(tablet_bits, word_idx);
    const int64_t bit_offset = emit_cursor_bits_ & 63;
    if (0 != bit_offset) {
      word &= (~static_cast<uint64_t>(0) << bit_offset);
    }
    if (0 == word) {
      while (++word_idx < word_cnt && 0 == (word = bitmap_word_at(tablet_bits, word_idx))) {
      }
      if (0 == word) {
        ++emit_tablet_idx_;
        emit_cursor_bits_ = 0;
        continue;
      }
    }
    const int64_t remain = capacity - count;
    const int64_t word_bit_cnt = __builtin_popcountll(word);
    if (word_bit_cnt <= remain) {
      // The whole word fits in this batch: count it and leave the cursor at the
      // head of the following word.
      count += word_bit_cnt;
      emit_cursor_bits_ = (word_idx + 1) << 6;
    } else {
      // The batch boundary falls inside this word: stop right after the
      // remain-th set bit so the next call resumes exactly there.
      int64_t taken = 0;
      while (taken < remain) {
        const int64_t bit = (word_idx << 6) + __builtin_ctzll(word);
        word &= word - 1;
        emit_cursor_bits_ = bit + 1;
        ++taken;
      }
      count += taken;
    }
  }
  output_row_cnt_ += count;
  input_row_cnt_ += count;
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
  } else if (OB_UNLIKELY(built_ && emit_tablet_idx_ >= tablet_bits_.count())) {
    ret = OB_ITER_END;
  } else {
    if (!built_) {
      if (OB_SUCC(ret) && !iter_param_->count_only_ && OB_ISNULL(emit_buffer_)) {
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
        emit_tablet_idx_ = 0;
        emit_cursor_bits_ = 0;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(emit_rows(capacity, count))) {
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase

