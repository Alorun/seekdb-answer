/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  float* query_device;
  unsigned int* ids_device;
  float* distances_device;
  size_t query_capacity;
  size_t result_capacity;
  cuvsCagraSearchParams_t search_params;
  size_t default_itopk;

} sk_cuvs_index;

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  h->ip->nn_descent_niter = 100;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

/* Calls on a handle are serialized by the adaptor. Retain scratch allocations
 * for that index, and release all of them with its cuVS resources. */
static int reserve_search_buffers(sk_cuvs_index* h, size_t query_count, size_t result_count)
{
  if (query_count > h->query_capacity) {
    float* replacement = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&replacement,
                     query_count * sizeof(float)) != CUVS_SUCCESS) {
      if (replacement) cuvsRMMFree(h->res, replacement, query_count * sizeof(float));
      return 10;
    }
    if (h->query_device) cuvsRMMFree(h->res, h->query_device,
                                     h->query_capacity * sizeof(float));
    h->query_device = replacement;
    h->query_capacity = query_count;
  }
  if (result_count > h->result_capacity) {
    unsigned int* ids = NULL;
    float* distances = NULL;
    if (cuvsRMMAlloc(h->res, (void**)&ids,
                     result_count * sizeof(unsigned int)) != CUVS_SUCCESS) {
      if (ids) cuvsRMMFree(h->res, ids, result_count * sizeof(unsigned int));
      return 11;
    }
    if (cuvsRMMAlloc(h->res, (void**)&distances,
                     result_count * sizeof(float)) != CUVS_SUCCESS) {
      cuvsRMMFree(h->res, ids, result_count * sizeof(unsigned int));
      if (distances) cuvsRMMFree(h->res, distances, result_count * sizeof(float));
      return 12;
    }
    if (h->ids_device) cuvsRMMFree(h->res, h->ids_device,
                                   h->result_capacity * sizeof(unsigned int));
    if (h->distances_device) cuvsRMMFree(h->res, h->distances_device,
                                         h->result_capacity * sizeof(float));
    h->ids_device = ids;
    h->distances_device = distances;
    h->result_capacity = result_count;
  }
  return 0;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h || !query || !out_ids || nq <= 0 || topk <= 0 || topk > h->n ||
      h->dim <= 0 || (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)h->dim ||
      (size_t)nq > SIZE_MAX / sizeof(float) / (size_t)topk ||
      (size_t)nq > SIZE_MAX / sizeof(unsigned int) / (size_t)topk ||
      (size_t)topk > SIZE_MAX / 4) return 1;
  const size_t query_count = (size_t)nq * (size_t)h->dim;
  const size_t result_count = (size_t)nq * (size_t)topk;
  int rc = reserve_search_buffers(h, query_count, result_count);
  if (rc != 0) return rc;
  if (!h->search_params) {
    cuvsCagraSearchParams_t params = NULL;
    if (cuvsCagraSearchParamsCreate(&params) != CUVS_SUCCESS) {
      if (params) cuvsCagraSearchParamsDestroy(params);
      return 14;
    }
    h->search_params = params;
    h->default_itopk = params->itopk_size;
  }
  {
    size_t required_itopk = (size_t)topk * 4;
    if (required_itopk < 512) required_itopk = 512;
    h->search_params->itopk_size = h->default_itopk > required_itopk
        ? h->default_itopk : required_itopk;
    h->search_params->search_width = 4;
  }
  if (cudaMemcpy(h->query_device, query, query_count * sizeof(float),
                   cudaMemcpyDefault) != cudaSuccess) return 13;
  DLManagedTensor qt = {0}, nt = {0}, dt = {0};
  long qs[2] = {nq, h->dim}, ns[2] = {nq, topk};
  qt.dl_tensor.data = h->query_device;
  qt.dl_tensor.device.device_type = kDLCUDA;
  qt.dl_tensor.device.device_id = 0;
  qt.dl_tensor.ndim = 2;
  qt.dl_tensor.dtype.code = kDLFloat;
  qt.dl_tensor.dtype.bits = 32;
  qt.dl_tensor.dtype.lanes = 1;
  qt.dl_tensor.shape = qs;
  nt = qt; nt.dl_tensor.data = h->ids_device;
  nt.dl_tensor.dtype.code = kDLUInt; nt.dl_tensor.shape = ns;
  dt = qt; dt.dl_tensor.data = h->distances_device; dt.dl_tensor.shape = ns;
  cuvsFilter filter; filter.type = NO_FILTER; filter.addr = (uintptr_t)0;
  if (cuvsCagraSearch(h->res, h->search_params, h->idx, &qt, &nt, &dt,
                      filter) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] search failed: %s\n", cuvsGetLastErrorText());
    return 15;
  }
  if (cudaMemcpy(out_ids, h->ids_device, result_count * sizeof(unsigned int),
                   cudaMemcpyDefault) != cudaSuccess) return 16;
  if (out_dist && cudaMemcpy(out_dist, h->distances_device,
                              result_count * sizeof(float),
                              cudaMemcpyDefault) != cudaSuccess) return 17;
  return 0;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  if (h->search_params) cuvsCagraSearchParamsDestroy(h->search_params);
  if (h->query_device) cuvsRMMFree(h->res, h->query_device,
                                   h->query_capacity * sizeof(float));
  if (h->ids_device) cuvsRMMFree(h->res, h->ids_device,
                                 h->result_capacity * sizeof(unsigned int));
  if (h->distances_device) cuvsRMMFree(h->res, h->distances_device,
                                       h->result_capacity * sizeof(float));
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = (sk_cuvs_index*)calloc(1, sizeof(*index));
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
