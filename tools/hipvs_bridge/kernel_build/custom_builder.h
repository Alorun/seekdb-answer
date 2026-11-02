#pragma once
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#ifdef __cplusplus
extern "C" {
#endif
/* flags: bit 0 hash pruning, bit 1 compact join barriers, bit 2 verify pruning.
 * No search implementation is supplied here. The result is a fixed-library index. */
int seekdb_cuvs_custom_build(cuvsResources_t res, const float* base,
                           long n, long dim, cuvsCagraIndex_t output, unsigned flags);
#ifdef __cplusplus
}
#endif
