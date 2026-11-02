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

#include "ob_sparse_taat_iter.h"

namespace oceanbase
{
namespace storage
{

ObSRTaaTIterImpl::ObSRTaaTIterImpl()
  : ObISparseRetrievalMergeIter(),
    iter_allocator_(nullptr),
    iter_param_(nullptr),
    dim_iter_(nullptr),
    partition_cnt_(0),
    datum_stores_(nullptr),
    datum_store_iters_(nullptr),
    skips_(nullptr),
    hash_maps_(nullptr),
    cur_map_iter_(nullptr),
    cur_map_idx_(-1),
    next_clear_map_idx_(0),
    cache_first_id_(),
    are_chunk_stores_inited_(false),
    are_chunk_stores_filled_(false),
    use_direct_accum_(false),
    direct_bucket_cnt_(0),
    set_datum_func_(nullptr)
{
}

int ObSRTaaTIterImpl::init(
    ObSparseRetrievalMergeParam &iter_param,
    ObISparseRetrievalDimIter &dim_iter,
    ObIAllocator &iter_allocator)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(iter_param.max_batch_size_ <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    iter_allocator_ = &iter_allocator;
    iter_param_ = &iter_param;
    dim_iter_ = &dim_iter;
    input_row_cnt_ = 0;
    output_row_cnt_ = 0;
    if (iter_param_->id_proj_expr_->datum_meta_.type_ == common::ObUInt64Type) {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_int;
    } else {
      set_datum_func_ = ObISparseRetrievalMergeIter::set_datum_shallow;
    }
    is_inited_ = true;
  }
  return ret;
}

void ObSRTaaTIterImpl::reset()
{
  ObSRTaaTIterImpl::reuse();
  partition_cnt_ = 0;
  is_inited_ = false;
}

void ObSRTaaTIterImpl::reuse(const bool switch_tablet)
{
  if (nullptr != hash_maps_) {
    for (int64_t i = 0; i < partition_cnt_; ++i) {
      // An entry can be null when init_chunk_stores() failed part way.
      if (OB_NOT_NULL(hash_maps_[i])) {
        hash_maps_[i]->destroy();
      }
    }
    hash_maps_ = nullptr;
  }
  if (nullptr != datum_stores_) {
    for (int64_t i = 0; i < partition_cnt_; ++i) {
      query::reset_spill_row_store(datum_stores_[i]);
    }
    datum_stores_ = nullptr;
  }
  if (nullptr != datum_store_iters_) {
    for (int64_t i = 0; i < partition_cnt_; ++i) {
      query::reset_spill_row_store_iterator(datum_store_iters_[i]);
    }
    datum_store_iters_ = nullptr;
  }
  skips_ = nullptr;
  cur_map_iter_ = nullptr;
  cur_map_idx_ = -1;
  next_clear_map_idx_ = 0;
  use_direct_accum_ = false;
  direct_bucket_cnt_ = 0;
  cache_first_id_.reset();
  are_chunk_stores_inited_ = false;
  are_chunk_stores_filled_ = false;
  input_row_cnt_ = 0;
  output_row_cnt_ = 0;
}

int ObSRTaaTIterImpl::get_next_row()
{
  int ret = OB_SUCCESS;
  int64_t count = 0;
  if (OB_FAIL(get_next_rows(1, count))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else if (OB_UNLIKELY(count != 0)) {
      ret = OB_ERR_UNEXPECTED;
    }
  } else if (OB_UNLIKELY(count != 1)) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObSRTaaTIterImpl::get_next_rows(const int64_t capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(0 == capacity)) {
    count = 0;
  } else if (iter_param_->limit_param_->is_valid() && output_row_cnt_ >= iter_param_->limit_param_->limit_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(pre_process())) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    } else {
      count = 0;
    }
  } else if (OB_UNLIKELY(0 == partition_cnt_)) {
    ret = OB_ITER_END;
  } else if (!are_chunk_stores_inited_ && OB_FAIL(init_chunk_stores())) {
  } else if (!are_chunk_stores_filled_ && OB_FAIL(fill_chunk_stores())) {
  } else if (cur_map_idx_ > partition_cnt_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    if (cur_map_idx_ == partition_cnt_) {
      ret = OB_ITER_END;
    }
    while (next_clear_map_idx_ < cur_map_idx_) {
      hash_maps_[next_clear_map_idx_++]->clear();
    }
    count = 0;
    const int64_t real_capacity = MIN(capacity, iter_param_->max_batch_size_);
    while (OB_SUCC(ret) && count < real_capacity) {
      if (-1 != cur_map_idx_ && nullptr != cur_map_iter_
          && *cur_map_iter_ != hash_maps_[cur_map_idx_]->end()) {
        if (OB_FAIL(project_results(real_capacity, count))) {
        }
      } else if (OB_FAIL(load_next_hash_map())) {
        if (OB_ITER_END != ret) {
        }
      }
    }

    if ((OB_SUCC(ret) || OB_ITER_END == ret) && count > 0) {
      ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
      ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx, 0);
      set_datum_func_(id_datum, cache_first_id_);
    }
  }
  return ret;
}

int ObSRTaaTIterImpl::pre_process()
{
  return OB_NOT_IMPLEMENT;
}

int ObSRTaaTIterImpl::init_chunk_stores()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(are_chunk_stores_inited_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(partition_cnt_ <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    void *buf = nullptr;
    int64_t capacity = iter_param_->max_batch_size_;
    if (nullptr == hash_maps_) {
      if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(ObSRTaaTHashMap *) * partition_cnt_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        hash_maps_ = static_cast<ObSRTaaTHashMap **>(buf);
        for (int64_t i = 0; i < partition_cnt_; ++i) {
          hash_maps_[i] = nullptr;
        }
      }
    }
    if (OB_SUCC(ret) && !use_direct_accum_ && nullptr == datum_stores_) {
      if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(query::ObSpillRowStore *) * partition_cnt_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        datum_stores_ = static_cast<query::ObSpillRowStore **>(buf);
        for (int64_t i = 0; i < partition_cnt_; ++i) {
          datum_stores_[i] = nullptr;
        }
      }
    }
    if (OB_SUCC(ret) && !use_direct_accum_ && nullptr == datum_store_iters_) {
      if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(query::ObSpillRowStoreIterator *) * partition_cnt_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        datum_store_iters_ = static_cast<query::ObSpillRowStoreIterator **>(buf);
        for (int64_t i = 0; i < partition_cnt_; ++i) {
          datum_store_iters_[i] = nullptr;
        }
      }
    }
    if (OB_SUCC(ret) && !use_direct_accum_
        && iter_param_->id_proj_expr_->is_batch_result() && nullptr == skips_) {
      if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(sql::ObBitVector *) * partition_cnt_))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        skips_ = static_cast<sql::ObBitVector **>(buf);
        for (int64_t i = 0; i < partition_cnt_; ++i) {
          skips_[i] = nullptr;
        }
      }
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < partition_cnt_; ++i) {
      if (nullptr == hash_maps_[i]) {
        if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(ObSRTaaTHashMap)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          hash_maps_[i] = new (buf) ObSRTaaTHashMap();
        }
      }
      // ObHashMap does not extend its bucket array (EXTEND_RATIO == 1), so
      // this is the final bucket count of the partition. create() rounds the
      // requested size up to the next prime (1000 -> 1543). Keep the value in
      // sync with the per-partition document budget used by
      // ObTextTaaTIter::pre_process.
      const int64_t bucket_cnt = use_direct_accum_
          ? OB_MAX(direct_bucket_cnt_, static_cast<int64_t>(OB_HASHMAP_DEFAULT_SIZE))
          : OB_HASHMAP_DEFAULT_SIZE;
      if (OB_SUCC(ret) && !hash_maps_[i]->created()
          && OB_FAIL(hash_maps_[i]->create(bucket_cnt, common::ObMemAttr("FTTaatMap")))) {
      }
      if (OB_SUCC(ret) && !use_direct_accum_ && nullptr == datum_stores_[i]
          && OB_FAIL(query::create_spill_row_store(*iter_allocator_, datum_stores_[i]))) {
      }
      if (OB_SUCC(ret) && !use_direct_accum_ && nullptr == datum_store_iters_[i]
          && OB_FAIL(query::create_spill_row_store_iterator(*iter_allocator_, datum_store_iters_[i]))) {
      }
      if (OB_SUCC(ret) && !use_direct_accum_
          && iter_param_->id_proj_expr_->is_batch_result() && nullptr == skips_[i]) {
        if (OB_ISNULL(buf = iter_allocator_->alloc(sql::ObBitVector::memory_size(capacity)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          sql::ObBitVector *skip = sql::to_bit_vector(buf);
          skip->init(capacity);
          skips_[i] = skip;
        }
      }
    }
    if (OB_SUCC(ret)) {
      are_chunk_stores_inited_ = true;
    }
  }
  return ret;
}

int ObSRTaaTIterImpl::fill_chunk_stores()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!are_chunk_stores_inited_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(are_chunk_stores_filled_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (use_direct_accum_) {
    ret = fill_hash_map_directly();
  } else {
    ObSEArray<ObExpr *, 2> exprs;
    if (OB_FAIL(exprs.push_back(iter_param_->id_proj_expr_))) {
    } else if (iter_param_->need_project_relevance()
        && OB_FAIL(exprs.push_back(iter_param_->relevance_expr_))) {
    }
    ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
    int64_t capacity = iter_param_->max_batch_size_;
    int64_t total_count = 0;

    for (int64_t dim_idx = 0; OB_SUCC(ret); ) {
      int64_t subtotal_count = 0;
      if (iter_param_->id_proj_expr_->is_batch_result()) {
        // batched
        while (OB_SUCC(ret)) {
          int64_t count = 0;
          if (OB_FAIL(dim_iter_->get_next_batch(capacity, count))) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
            } else if (OB_LIKELY(count > 0)) {
              ret = OB_SUCCESS;
            }
          }
          if (OB_SUCC(ret)) {
            for (int64_t i = 0; i < partition_cnt_; ++i) {
              skips_[i]->set_all(capacity);
            }
            for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
              const ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx, i);
              if (OB_UNLIKELY(id_datum.is_null()) || OB_ISNULL(id_datum.ptr_)) {
                ret = OB_ERR_UNEXPECTED;
              } else {
                uint64_t partition = murmurhash(id_datum.ptr_, id_datum.len_, 0) % partition_cnt_;
                skips_[partition]->unset(i);
              }
            }
            int64_t check_count = 0;
            for (int64_t i = 0; OB_SUCC(ret) && i < partition_cnt_; ++i) {
              int64_t stored_count = 0;
              if (OB_FAIL(query::spill_row_store_add_batch(
                  datum_stores_[i], exprs, *eval_ctx, *skips_[i], count,
                  stored_count))) {
              } else {
                check_count += stored_count;
                subtotal_count += stored_count;
              }
            }
            if (OB_SUCC(ret) && count != check_count) {
              ret = OB_ERR_UNEXPECTED;
            }
          }
        }
      } else {
        // non-batched
        while (OB_SUCC(ret)) {
          if (OB_FAIL(dim_iter_->get_next_row())) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
            }
          } else {
            const ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx);
            if (OB_UNLIKELY(id_datum.is_null()) || OB_ISNULL(id_datum.ptr_)) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              uint64_t partition = murmurhash(id_datum.ptr_, id_datum.len_, 0) % partition_cnt_;
              if (OB_FAIL(query::spill_row_store_add_row(
                  datum_stores_[partition], exprs, *eval_ctx))) {
              } else {
                ++subtotal_count;
              }
            }
          }
        }
      }

      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      if (OB_SUCC(ret)) {
        total_count += subtotal_count;
        eval_ctx->reuse(eval_ctx->get_batch_size());
        if (OB_FAIL(update_dim_iter(++dim_idx))) {
          if (OB_ITER_END != ret) {
          }
        }
      }
    }

    if (OB_LIKELY(OB_ITER_END == ret)) {
      ret = OB_SUCCESS;
      int64_t check_count = 0;
      for (int64_t i = 0; OB_SUCC(ret) && i < partition_cnt_; ++i) {
        if (OB_FAIL(query::init_spill_row_store_iterator(
            datum_store_iters_[i], datum_stores_[i]))) {
        } else {
          check_count += query::spill_row_store_row_count(datum_stores_[i]);
        }
      }
      if (OB_SUCC(ret) && OB_UNLIKELY(total_count != check_count)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  if (OB_SUCC(ret)) {
    are_chunk_stores_filled_ = true;
  }
  return ret;
}

namespace {
// Adds one posting row into the per-partition score map.  set_or_update keeps
// the existing node and lets us fold the score in place, instead of the
// get_refactored + set_refactored pair which rebuilds the node on every update.
struct ObTaaTScoreAccumulator
{
  explicit ObTaaTScoreAccumulator(const double relevance) : relevance_(relevance) {}
  template <class PairType>
  void operator()(PairType &pair) { pair.second += relevance_; }
  double relevance_;
};
} // namespace

int ObSRTaaTIterImpl::accumulate_doc_id(
    ObSRTaaTHashMap &map,
    const ObDatum &id_datum,
    const double relevance)
{
  int ret = OB_SUCCESS;
  ObDocIdExt id;
  if (OB_UNLIKELY(id_datum.is_null()) || OB_ISNULL(id_datum.ptr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(id.from_datum(id_datum))) {
  } else if (iter_param_->need_project_relevance()) {
    ObTaaTScoreAccumulator accumulator(relevance);
    if (OB_FAIL(map.set_or_update(id, relevance, accumulator))) {
    }
  } else if (OB_FAIL(map.set_refactored(id, 1.0, 0 /* overwrite */))
             && OB_HASH_EXIST != ret) {
  } else {
    // A document matched by more than one dimension is already counted.
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObSRTaaTIterImpl::fill_hash_map_directly()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!are_chunk_stores_inited_) || OB_UNLIKELY(1 != partition_cnt_)
      || OB_ISNULL(hash_maps_) || OB_ISNULL(hash_maps_[0])) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObSRTaaTHashMap *map = hash_maps_[0];
    ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
    const int64_t capacity = iter_param_->max_batch_size_;
    const bool need_relevance = iter_param_->need_project_relevance();
    for (int64_t dim_idx = 0; OB_SUCC(ret); ) {
      if (iter_param_->id_proj_expr_->is_batch_result()) {
        while (OB_SUCC(ret)) {
          int64_t count = 0;
          if (OB_FAIL(dim_iter_->get_next_batch(capacity, count))) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
            } else if (OB_LIKELY(count > 0)) {
              ret = OB_SUCCESS;
            }
          }
          if (OB_SUCC(ret)) {
            for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
              const ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx, i);
              double relevance = 1.0;
              if (need_relevance) {
                relevance = iter_param_->relevance_expr_->locate_expr_datum(*eval_ctx, i).get_double();
              }
              if (OB_FAIL(accumulate_doc_id(*map, id_datum, relevance))) {
              }
            }
          }
        }
      } else {
        while (OB_SUCC(ret)) {
          if (OB_FAIL(dim_iter_->get_next_row())) {
            if (OB_UNLIKELY(OB_ITER_END != ret)) {
            }
          } else {
            const ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx);
            double relevance = 1.0;
            if (need_relevance) {
              relevance = iter_param_->relevance_expr_->locate_expr_datum(*eval_ctx).get_double();
            }
            if (OB_FAIL(accumulate_doc_id(*map, id_datum, relevance))) {
            }
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      if (OB_SUCC(ret)) {
        eval_ctx->reuse(eval_ctx->get_batch_size());
        if (OB_FAIL(update_dim_iter(++dim_idx))) {
          if (OB_ITER_END != ret) {
          }
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ObSRTaaTIterImpl::load_next_hash_map()
{
  int ret = OB_SUCCESS;
  ++cur_map_idx_;
  if (cur_map_idx_ >= partition_cnt_) {
    ret = OB_ITER_END;
  } else if (OB_UNLIKELY(hash_maps_[cur_map_idx_]->size() != 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(iter_param_->id_proj_expr_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (use_direct_accum_) {
    // The map has already been filled straight from the posting rows, there is
    // nothing to replay from a spill row store.
  } else {
    ObSRTaaTHashMap *map = hash_maps_[cur_map_idx_];
    query::ObSpillRowStoreIterator *store_iter = datum_store_iters_[cur_map_idx_];

    ObSEArray<ObExpr *, 2> exprs;
    if (OB_FAIL(exprs.push_back(iter_param_->id_proj_expr_))) {
    } else if (iter_param_->need_project_relevance()
        && OB_FAIL(exprs.push_back(iter_param_->relevance_expr_))) {
    }
    ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
    ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
    guard.set_batch_idx(0);

    ObDocIdExt id;
    double cur_relevance = 0.0;
    double last_relevance = 0.0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(query::spill_row_store_next_row(
          store_iter, *eval_ctx, exprs))) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
        }
      } else {
        ObDatum &id_datum = iter_param_->id_proj_expr_->locate_expr_datum(*eval_ctx);
        if (OB_FAIL(id.from_datum(id_datum))) {
        } else if (iter_param_->need_project_relevance()) {
          ObDatum &relevance_datum = iter_param_->relevance_expr_->locate_expr_datum(*eval_ctx);
          cur_relevance = relevance_datum.get_double();
          if (OB_FAIL(map->get_refactored(id, last_relevance))) {
            if (OB_HASH_NOT_EXIST != ret) {
            } else if (OB_FAIL(map->set_refactored(id, cur_relevance, 1 /* overwrite */))) {
            }
          } else if (OB_FAIL(map->set_refactored(id, cur_relevance + last_relevance, 1 /* overwrite */))) {
          }
        } else {
          if (OB_FAIL(map->set_refactored(id, 1.0, 0 /* overwrite */))) {
          }
        }
      }
    }
    if (OB_ITER_END == ret) {
      ret = OB_SUCCESS;
    }
  }
  if (OB_SUCC(ret)) {
    ObSRTaaTHashMap::iterator map_iter = hash_maps_[cur_map_idx_]->begin();
    void *buf = nullptr;
    if (nullptr != cur_map_iter_) {
      cur_map_iter_ = new (cur_map_iter_) ObSRTaaTHashMap::iterator(map_iter);
    } else if (OB_ISNULL(buf = iter_allocator_->alloc(sizeof(ObSRTaaTHashMap::iterator)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      cur_map_iter_ = new (buf) ObSRTaaTHashMap::iterator(map_iter);
    }
  }
  return ret;
}

int ObSRTaaTIterImpl::project_results(const int64_t safe_capacity, int64_t &count)
{
  int ret = OB_SUCCESS;
  ObEvalCtx *eval_ctx = iter_param_->eval_ctx_;
  ObDatum *id_datum = iter_param_->id_proj_expr_->locate_batch_datums(*eval_ctx);
  ObExpr *relevance_expr = iter_param_->relevance_proj_expr_;
  ObDatum *relevance_datum = nullptr;
  ObExpr *filter_expr = iter_param_->filter_expr_;
  const ObLimitParam *limit_param = iter_param_->limit_param_;
  if (iter_param_->need_project_relevance()) {
    if (count == 0) {
      relevance_datum = relevance_expr->locate_datums_for_update(*eval_ctx, safe_capacity);
    } else {
      relevance_datum = relevance_expr->locate_batch_datums(*eval_ctx);
    }
  }
  const bool need_filter = iter_param_->need_filter();
  // The batch index only matters for the per-row filter expression, so the
  // guard is created once per batch instead of once per projected row.
  ObEvalCtx::BatchInfoScopeGuard guard(*eval_ctx);
  for (; OB_SUCC(ret) && count < safe_capacity && *cur_map_iter_ != hash_maps_[cur_map_idx_]->end(); ++(*cur_map_iter_)) {
    if (need_filter) {
      guard.set_batch_idx(count);
    }
    set_datum_func_(id_datum[count], (*cur_map_iter_)->first);
    if (iter_param_->need_project_relevance()) {
      relevance_datum[count].set_double((*cur_map_iter_)->second);
      relevance_expr->set_evaluated_flag(*eval_ctx);
    }
    if (0 == count) {
      cache_first_id_ = (*cur_map_iter_)->first;
    }

    bool need_project = true;
    ObDatum *filter_res = nullptr;
    if (need_filter) {
      filter_expr->clear_evaluated_flag(*eval_ctx);
      if (OB_FAIL(filter_expr->eval(*eval_ctx, filter_res))) {
      } else {
        need_project = !(filter_res->is_null() || 0 == filter_res->get_int());
      }
    }

    if (OB_SUCC(ret) && need_project) {
      ++input_row_cnt_;
      if (limit_param->is_valid() && input_row_cnt_ <= limit_param->offset_) {
        // don't need to project, will be overwritten
      } else {
        ++output_row_cnt_;
        ++count;
        if (limit_param->is_valid() && output_row_cnt_ >= limit_param->limit_) {
          ret = OB_ITER_END;
        }
      }
    }
  }
  if (OB_SUCC(ret) && iter_param_->need_project_relevance()) {
    relevance_expr->set_evaluated_projected(*eval_ctx);
  }
  return ret;
}

int ObSRTaaTIterImpl::update_dim_iter(const int64_t dim_idx)
{
  return OB_NOT_IMPLEMENT;
}

} // namespace storage
} // namespace oceanbase
