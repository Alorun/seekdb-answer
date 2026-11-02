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

#ifndef OB_SPARSE_TAAT_ITER_H_
#define OB_SPARSE_TAAT_ITER_H_

#include "query/engine/basic/ob_spill_row_store.h"
#include "ob_i_sparse_retrieval_iter.h"

namespace oceanbase
{
namespace storage
{

// implementation of basic TaaT query processing algorithm primitives
class ObSRTaaTIterImpl : public ObISparseRetrievalMergeIter
{
public:
  ObSRTaaTIterImpl();
  virtual ~ObSRTaaTIterImpl() {}
  virtual int get_next_row() override;
  virtual int get_next_rows(const int64_t capacity, int64_t &count) override;
  int init(
      ObSparseRetrievalMergeParam &iter_param,
      ObISparseRetrievalDimIter &dim_iter,
      ObIAllocator &iter_allocator);
  virtual void reuse(const bool switch_tablet = false) override;
  virtual void reset() override;
  INHERIT_TO_STRING_KV("ObISparseRetrievalMergeIter", ObISparseRetrievalMergeIter,
      K_(partition_cnt), K_(cur_map_idx), K_(next_clear_map_idx));
protected:
  typedef hash::ObHashMap<ObDocIdExt, double> ObSRTaaTHashMap;
  static const int64_t OB_MAX_HASHMAP_COUNT = 20;
  static const int64_t OB_HASHMAP_DEFAULT_SIZE = 1000;
  // Upper bound on the document count for which the whole per-tablet result is
  // merged in a single hash map.  Above it the partitioned spill row store path
  // keeps the peak memory bounded for very large indexes.
  static const int64_t OB_TAAT_DIRECT_MAX_DOC_CNT = 128 * 1024;
protected:
  virtual int pre_process(); // should set partition_cnt_, among other things
  virtual int init_chunk_stores();
  virtual int fill_chunk_stores();
  // Accumulate the posting rows straight into the per-partition hash maps.
  // Used when the partitions fit in a single map, which makes the spill row
  // stores unnecessary: they only added one serialize/deserialize round trip
  // per posting row.
  int fill_hash_map_directly();
  int accumulate_doc_id(ObSRTaaTHashMap &map, const common::ObDatum &id_datum, const double relevance);
  virtual int load_next_hash_map();
  virtual int project_results(const int64_t safe_capacity, int64_t &count);
  virtual int update_dim_iter(const int64_t dim_idx); // should return OB_ITER_END if no more dimensions
protected:
  ObIAllocator *iter_allocator_;
  ObSparseRetrievalMergeParam *iter_param_;
  ObISparseRetrievalDimIter *dim_iter_;
  int64_t partition_cnt_;
  query::ObSpillRowStore **datum_stores_;
  query::ObSpillRowStoreIterator **datum_store_iters_;
  sql::ObBitVector **skips_;
  ObSRTaaTHashMap **hash_maps_;
  ObSRTaaTHashMap::iterator *cur_map_iter_;
  int64_t cur_map_idx_;
  int64_t next_clear_map_idx_;
  ObDocIdExt cache_first_id_;
  bool are_chunk_stores_inited_;
  bool are_chunk_stores_filled_;
  // Set by pre_process() when the whole result of this tablet fits into one
  // hash map, so posting rows can be merged without the spill row store.
  bool use_direct_accum_;
  int64_t direct_bucket_cnt_;
  void (*set_datum_func_)(ObDatum &, const ObDocIdExt &);
private:
  DISALLOW_COPY_AND_ASSIGN(ObSRTaaTIterImpl);
};

} // namespace storage
} // namespace oceanbase

#endif
