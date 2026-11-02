#ifndef SEEKDB_CUVS_BRIDGE_H
#define SEEKDB_CUVS_BRIDGE_H
#define SK_BRIDGE_API __attribute__((visibility("default")))
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
SK_BRIDGE_API int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * Search scratch buffers are retained until seekdb_cuvs_free. Calls using the
 * same handle must be serialized; the seekdb adaptor already provides this.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
SK_BRIDGE_API void* seekdb_cuvs_build(const float* base, long n, long dim);
SK_BRIDGE_API int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
SK_BRIDGE_API void  seekdb_cuvs_free(void* handle);
SK_BRIDGE_API int seekdb_cuvs_serialize(void* handle, const char* filename);
SK_BRIDGE_API void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);
#ifdef __cplusplus
}
#endif
#endif
