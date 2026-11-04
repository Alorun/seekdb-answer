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

#include "data_plane/fts/ob_fts_struct.h"

#include <cstdlib>

#include "lib/charset/ob_charset.h"
#include "share/datum/ob_datum_funcs.h"
#include "storage/ob_storage_util.h"

namespace oceanbase
{
namespace storage
{

namespace
{
// The collation aware hash of a word is by far the most expensive part of a word
// map operation: utf8mb4_general_ci walks the string through the collation
// weights for every byte.  The vocabulary of a document repeats heavily (and so
// does the vocabulary of the next document), so the hash of recently seen words
// is memoized.  The cache is keyed by the whole token and by the collation, and
// a hit returns exactly the value the collation hash would return, so the bucket
// of a word - and therefore the iteration order of the map - cannot change.
// OB_FTS_WORD_HASH_CACHE=0 disables it.
constexpr int64_t WORD_HASH_CACHE_SLOTS = 512;
constexpr int64_t WORD_HASH_CACHE_MAX_LEN = 24;

bool word_hash_cache_enabled()
{
  static const bool enabled = []() -> bool {
    const char *value = ::getenv("OB_FTS_WORD_HASH_CACHE");
    return !(nullptr != value && '0' == value[0]);
  }();
  return enabled;
}

struct ObWordHashCacheEntry
{
  uint64_t hash_;
  int32_t len_;
  int32_t cs_;
  bool valid_;
  char bytes_[WORD_HASH_CACHE_MAX_LEN];
};

ObWordHashCacheEntry *word_hash_cache()
{
  static thread_local ObWordHashCacheEntry cache[WORD_HASH_CACHE_SLOTS];
  return cache;
}

// Cheap index: the length plus the first and last four bytes of the token.  Two
// different words that land in the same slot simply evict each other.
inline uint32_t word_hash_cache_slot(const char *ptr, const int64_t len)
{
  uint32_t h = static_cast<uint32_t>(len) * 2654435761u;
  if (len >= 4) {
    uint32_t first = 0;
    uint32_t last = 0;
    MEMCPY(&first, ptr, sizeof(first));
    MEMCPY(&last, ptr + len - 4, sizeof(last));
    h ^= first * 2246822519u;
    h ^= last * 3266489917u;
  } else {
    for (int64_t i = 0; i < len; ++i) {
      h = (h ^ static_cast<unsigned char>(ptr[i])) * 16777619u;
    }
  }
  return (h ^ (h >> 15)) & (WORD_HASH_CACHE_SLOTS - 1);
}

// Inline equality: the tokens are short, a libc memcmp call would cost more than
// the comparison itself.
inline bool word_bytes_equal(const char *lhs, const char *rhs, const int64_t len)
{
  int64_t i = 0;
  for (; i + 8 <= len; i += 8) {
    uint64_t a = 0;
    uint64_t b = 0;
    MEMCPY(&a, lhs + i, sizeof(a));
    MEMCPY(&b, rhs + i, sizeof(b));
    if (a != b) {
      return false;
    }
  }
  for (; i < len; ++i) {
    if (lhs[i] != rhs[i]) {
      return false;
    }
  }
  return true;
}
} // namespace

int ObFTWord::hash(uint64_t &hash_val) const
{
  int ret = OB_SUCCESS;
  const ObString str = word_.get_string();
  const int64_t len = str.length();
  const int32_t cs = static_cast<int32_t>(meta_.get_collation_type());
  const bool cacheable = word_hash_cache_enabled() && !word_.is_null() && len > 0
                         && len <= WORD_HASH_CACHE_MAX_LEN;
  uint32_t slot = 0;
  bool hit = false;
  if (OB_LIKELY(cacheable)) {
    slot = word_hash_cache_slot(str.ptr(), len);
    const ObWordHashCacheEntry &entry = word_hash_cache()[slot];
    if (entry.valid_ && entry.len_ == static_cast<int32_t>(len) && entry.cs_ == cs
        && word_bytes_equal(entry.bytes_, str.ptr(), len)) {
      hash_val = entry.hash_;
      hit = true;
    }
  }
  if (!hit) {
    common::ObDatumBasicFuncs *funcs
        = ObDatumFuncs::get_basic_func(meta_.get_type(), meta_.get_collation_type());
    if (OB_ISNULL(funcs)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (funcs->default_hash_ == nullptr) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(funcs->default_hash_(word_, 0, hash_val, nullptr))) {
    } else if (cacheable) {
      ObWordHashCacheEntry &entry = word_hash_cache()[slot];
      MEMCPY(entry.bytes_, str.ptr(), len);
      entry.len_ = static_cast<int32_t>(len);
      entry.cs_ = cs;
      entry.hash_ = hash_val;
      entry.valid_ = true;
    }
  }
  return ret;
}
namespace
{
// get_datum_cmp_func() resolves the comparison function from the two metas on
// every comparison; the same pair of metas is used by a whole document, so the
// last few resolutions are memoised.  A hit returns the very same function
// pointer, which cannot change the comparison result.
constexpr int64_t CMP_FUNC_MEMO_SLOTS = 4;

struct ObCmpFuncMemoEntry
{
  ObDatumCmpFuncType func_;
  int32_t type1_;
  int32_t cs1_;
  int32_t type2_;
  int32_t cs2_;
  bool valid_;
};

ObDatumCmpFuncType memoized_cmp_func(const ObObjMeta &meta1, const ObObjMeta &meta2)
{
  static thread_local ObCmpFuncMemoEntry memo[CMP_FUNC_MEMO_SLOTS];
  static thread_local int64_t next = 0;
  const int32_t t1 = meta1.get_type();
  const int32_t c1 = meta1.get_collation_type();
  const int32_t t2 = meta2.get_type();
  const int32_t c2 = meta2.get_collation_type();
  for (int64_t i = 0; i < CMP_FUNC_MEMO_SLOTS; ++i) {
    const ObCmpFuncMemoEntry &entry = memo[i];
    if (entry.valid_ && entry.type1_ == t1 && entry.cs1_ == c1 && entry.type2_ == t2
        && entry.cs2_ == c2) {
      return entry.func_;
    }
  }
  const ObDatumCmpFuncType func = get_datum_cmp_func(meta1, meta2);
  ObCmpFuncMemoEntry &entry = memo[next];
  next = (next + 1) % CMP_FUNC_MEMO_SLOTS;
  entry.func_ = func;
  entry.type1_ = t1;
  entry.cs1_ = c1;
  entry.type2_ = t2;
  entry.cs2_ = c2;
  entry.valid_ = true;
  return func;
}
} // namespace

bool ObFTWord::operator==(const ObFTWord &other) const
{
  bool is_equal = false;
  int ret = OB_SUCCESS;
  int cmp_ret = 0;
  ObDatumCmpFuncType func = memoized_cmp_func(meta_, other.meta_);
  if (func == nullptr) {
    ob_abort();
  } else if (OB_FAIL(func(word_, other.word_, cmp_ret, nullptr))) {
    ob_abort();
  } else {
    is_equal = (cmp_ret == 0);
  }
  return is_equal;
}
} // namespace storage
} // namespace oceanbase
