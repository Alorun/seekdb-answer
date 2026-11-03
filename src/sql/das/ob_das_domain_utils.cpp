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

#include <cstdlib>
#include <unistd.h>
#include <sched.h>
#include <atomic>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "config_bridge.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/hash_func/murmur_hash.h"
#include "lib/lock/ob_lock_guard.h"
#include "lib/lock/ob_mutex.h"
#include "lib/time/ob_time_utility.h"
#include "common/json_type/ob_json_bin.h"
#include "share/ob_errno.h"
#define USING_LOG_PREFIX SQL_DAS

#include "ob_das_domain_utils.h"
#include "data_plane/blocksstable/ob_datum_row.h"
#include "data_plane/access/ob_datum_reshape.h"
#include "share/geo/ob_geo_utils.h"
#include "sql/das/ob_das_dml_vec_iter.h"
#include "sql/engine/expr/ob_expr_lob_utils.h"
#include "share/geo/ob_srs_provider.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{
using storage::ObFTWordMap;


ObObjDatumMapType ObFTIndexRowCache::FTS_INDEX_TYPES[] = {OBJ_DATUM_STRING, OBJ_DATUM_STRING, OBJ_DATUM_8BYTE_DATA, OBJ_DATUM_8BYTE_DATA};
ObObjDatumMapType ObFTIndexRowCache::FTS_DOC_WORD_TYPES[] = {OBJ_DATUM_STRING, OBJ_DATUM_STRING, OBJ_DATUM_8BYTE_DATA, OBJ_DATUM_8BYTE_DATA};

ObExprOperatorType ObFTIndexRowCache::FTS_INDEX_EXPR_TYPE[] = {T_FUN_SYS_WORD_SEGMENT, T_FUN_SYS_DOC_ID, T_FUN_SYS_WORD_COUNT, T_FUN_SYS_DOC_LENGTH};
ObExprOperatorType ObFTIndexRowCache::FTS_DOC_WORD_EXPR_TYPE[] = {T_FUN_SYS_DOC_ID, T_FUN_SYS_WORD_SEGMENT, T_FUN_SYS_WORD_COUNT, T_FUN_SYS_DOC_LENGTH};

namespace
{

// ---------------------------------------------------------------------------
// Shared tokenization cache: one tokenization feeds both fts aux tables.
//
// A fulltext index stores its data in two tokenized aux tables: `fts_index`
// ([word, docid, word_count, doc_len], primary key (word, docid)) and
// `fts_doc_word` ([docid, word, word_count, doc_len], primary key (docid, word)).
// They are built by two independent DDL sub tasks, each running
// `INSERT ... SELECT ... FROM <data table>` with its own target table order, so
// today every document text is scanned and tokenized twice.  The two builds run
// concurrently, which is why they cannot share one scan without restructuring
// the PX plan into two ordered branches.
//
// Tokenization itself is a pure function of (text, parser name + properties,
// collation), so the second build can reuse the first build's result.  This
// cache is that reuse point: it is content addressed (the document text is part
// of the key and is verified with memcmp on every hit), so a hit can only return
// the tokenization of exactly the requested text.  A collision or an eviction
// simply costs a cache miss, never a wrong row.  The cache is bounded by
// `_fts_token_share_mem_limit_mb` and sharded to keep contention low; every
// entry is only a copy of tokens the caller already has in memory, so it cannot
// leak data between tables or tenants.
// ---------------------------------------------------------------------------

const int64_t FTS_TOKEN_SHARE_SHARD_CNT = 16;
const int64_t FTS_TOKEN_SHARE_MIN_SHARD_BUCKETS = 1024;
const int64_t FTS_TOKEN_SHARE_MAX_SHARD_BUCKETS = 16384;
const int64_t FTS_TOKEN_SHARE_BUCKET_BYTES = 1024;
const int64_t FTS_TOKEN_SHARE_MAX_TEXT_LEN = 1L << 20;
const int64_t FTS_TOKEN_SHARE_LOG_INTERVAL = 10000;
// The two tokenized aux builds of one index run concurrently and stay in
// lockstep, so a pure lookup cache would almost always observe both sides
// segmenting the same document at the same time.  The first thread to reach a
// text therefore announces that it is tokenizing it, and the other one waits
// (instead of doing the same work twice) until the result is published.
const int64_t FTS_TOKEN_SHARE_INFLIGHT_SLOTS = 64;
const int64_t FTS_TOKEN_SHARE_INFLIGHT_TTL_US = 2000000;
const int64_t FTS_TOKEN_SHARE_WAIT_RETRY = 32;
// Linux rounds short sleeps up to the per-thread timer slack (50us by default,
// which is longer than one document's tokenization and would make a waiter the
// build's critical path).  The threads that wait use a 1us slack so a waiter
// wakes up a few microseconds after the leader published, i.e. it waits about as
// long as it would have spent tokenizing the same document itself.
const int64_t FTS_TOKEN_SHARE_TIMER_SLACK_NS = 1000;
const int64_t FTS_TOKEN_SHARE_WAIT_MAX_SLEEP_US = 16;
// If waiting never pays off (e.g. a stalled leader), stop waiting after this many
// consecutive documents so a pathological case can only slow one document at a
// time instead of the whole build.
const int64_t FTS_TOKEN_SHARE_MAX_CONSECUTIVE_WAITS = 64;
const uint64_t FTS_TOKEN_SHARE_HASH_SEED = 0x9e3779b97f4a7c15ULL;

struct FtsSharedToken
{
  const char *ptr_;
  int64_t cnt_;
  int32_t len_;
  int32_t reserved_;
};

struct FtsSharedDoc
{
  FtsSharedDoc() : doc_length_(0), token_cnt_(0), tokens_(nullptr) {}
  int64_t doc_length_;
  int64_t token_cnt_;
  FtsSharedToken *tokens_;
};

// One cached tokenization.  `payload_` is a single block laid out as
// [FtsSharedToken * token_cnt_][text bytes][word bytes]; the token entries point
// into the word bytes of the same block.
struct FtsShareEntry
{
  FtsShareEntry()
    : hash_(0), parser_sig_(0), collation_(0), text_len_(0), doc_length_(0),
      token_cnt_(0), word_bytes_(0), size_(0), payload_(nullptr), text_(nullptr),
      older_(nullptr), newer_(nullptr), bucket_next_(nullptr), bucket_prev_(nullptr)
  {}
  uint64_t hash_;
  uint64_t parser_sig_;
  int32_t collation_;
  int32_t text_len_;
  int64_t doc_length_;
  int64_t token_cnt_;
  int64_t word_bytes_;
  int64_t size_;
  char *payload_;
  const char *text_;
  FtsShareEntry *older_;
  FtsShareEntry *newer_;
  FtsShareEntry *bucket_next_;
  FtsShareEntry *bucket_prev_;
};

class FtsTokenShareCache final
{
public:
  static FtsTokenShareCache &instance()
  {
    static FtsTokenShareCache cache;
    return cache;
  }

  // Returns OB_SUCCESS when the exact same text is cached; the tokenization is
  // copied into `allocator` so the caller owns stable memory.  Every other
  // outcome (miss, allocation failure, disabled cache) is reported as an error
  // and must be treated as a miss by the caller.
  int fetch(const uint64_t hash, const uint64_t parser_sig, const int32_t collation,
            const common::ObString &text, common::ObIAllocator &allocator,
            FtsSharedDoc &doc);
  void put(const uint64_t hash, const uint64_t parser_sig, const int32_t collation,
           const common::ObString &text, const int64_t doc_length,
           const storage::ObFTWordMap &word_map);

  // Leader election for one text: LEAD means this thread announces that it is
  // tokenizing the text (others wait), WAIT means another thread is already on
  // it, UNTRACKED means the announcement table is full and the caller simply
  // does the work without coordination.
  enum LeadResult { LEAD = 0, WAIT = 1, UNTRACKED = 2 };
  int try_lead(const uint64_t hash, LeadResult &result);
  void release_lead(const uint64_t hash);

private:
  struct InflightSlot
  {
    InflightSlot() : hash_(0), parser_sig_(0), ctime_us_(0) {}
    uint64_t hash_;
    uint64_t parser_sig_;
    int64_t ctime_us_;
  };
  struct Shard
  {
    Shard() : mutex_(), buckets_(nullptr), bucket_cnt_(0), bytes_(0), budget_(0),
              newest_(nullptr), oldest_(nullptr) {}
    lib::ObMutex mutex_;
    FtsShareEntry **buckets_;
    int64_t bucket_cnt_;
    int64_t bytes_;
    int64_t budget_;
    FtsShareEntry *newest_;
    FtsShareEntry *oldest_;
    InflightSlot inflight_[FTS_TOKEN_SHARE_INFLIGHT_SLOTS];
  };

  FtsTokenShareCache()
    : limit_bytes_(0), inited_(false), lookups_(0), hits_(0), stores_(0),
      evictions_(0), failed_(0) {}
  ~FtsTokenShareCache();

  int ensure_inited();
  FtsShareEntry *find(Shard &shard, const uint64_t hash) const;
  static bool matches(const FtsShareEntry &entry, const uint64_t parser_sig,
                      const int32_t collation, const common::ObString &text);
  static void lru_unlink(Shard &shard, FtsShareEntry &entry);
  static void lru_link_newest(Shard &shard, FtsShareEntry &entry);
  static void bucket_remove(Shard &shard, FtsShareEntry &entry);
  static void free_entry(FtsShareEntry *entry);
  static int copy_out(common::ObIAllocator &allocator, const FtsShareEntry &entry,
                      FtsSharedDoc &doc);
  void evict_if_needed(Shard &shard);
  void report();

private:
  Shard shards_[FTS_TOKEN_SHARE_SHARD_CNT];
  int64_t limit_bytes_;
  std::atomic<bool> inited_;
  std::atomic<int64_t> lookups_;
  std::atomic<int64_t> hits_;
  std::atomic<int64_t> stores_;
  std::atomic<int64_t> evictions_;
  std::atomic<int64_t> failed_;
};

FtsTokenShareCache::~FtsTokenShareCache()
{
  if (nullptr != shards_[0].buckets_) {
    for (int64_t i = 0; i < FTS_TOKEN_SHARE_SHARD_CNT; ++i) {
      Shard &shard = shards_[i];
      FtsShareEntry *entry = shard.oldest_;
      while (nullptr != entry) {
        FtsShareEntry *next = entry->newer_;
        free_entry(entry);
        entry = next;
      }
      if (nullptr != shard.buckets_) {
        ob_free(shard.buckets_);
        shard.buckets_ = nullptr;
      }
    }
  }
}

int FtsTokenShareCache::ensure_inited()
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(inited_.load(std::memory_order_acquire))) {
    // already attempted
  } else {
    static lib::ObMutex init_mutex;
    lib::ObLockGuard<lib::ObMutex> guard(init_mutex);
    if (!inited_.load(std::memory_order_relaxed)) {
      int64_t limit_mb = config::_fts_token_share_mem_limit_mb();
      if (limit_mb < 0) {
        limit_mb = 0;
      }
      limit_bytes_ = limit_mb << 20;
      if (limit_bytes_ > 0) {
        const int64_t shard_budget = limit_bytes_ / FTS_TOKEN_SHARE_SHARD_CNT;
        int64_t bucket_cnt = MAX(FTS_TOKEN_SHARE_MIN_SHARD_BUCKETS,
                                 MIN(FTS_TOKEN_SHARE_MAX_SHARD_BUCKETS,
                                     shard_budget / FTS_TOKEN_SHARE_BUCKET_BYTES));
        int64_t pow2 = FTS_TOKEN_SHARE_MIN_SHARD_BUCKETS;
        while (pow2 * 2 <= bucket_cnt) {
          pow2 *= 2;
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < FTS_TOKEN_SHARE_SHARD_CNT; ++i) {
          Shard &shard = shards_[i];
          shard.budget_ = shard_budget;
          shard.bucket_cnt_ = pow2;
          if (OB_ISNULL(shard.buckets_ = static_cast<FtsShareEntry **>(
                ob_malloc(sizeof(FtsShareEntry *) * pow2, ObMemAttr("FtsTokShare"))))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMSET(shard.buckets_, 0, sizeof(FtsShareEntry *) * pow2);
          }
        }
        if (OB_FAIL(ret)) {
          LOG_WARN("failed to allocate fts token share cache, token sharing disabled", K(ret));
          limit_bytes_ = 0;
        } else {
          LOG_INFO("fts token share cache enabled", K(limit_mb), K(shard_budget), K(pow2));
        }
      }
      inited_.store(true, std::memory_order_release);
    }
  }
  if (0 == limit_bytes_) {
    ret = OB_ENTRY_NOT_EXIST;
  }
  return ret;
}

FtsShareEntry *FtsTokenShareCache::find(Shard &shard, const uint64_t hash) const
{
  FtsShareEntry *entry = nullptr;
  if (OB_NOT_NULL(shard.buckets_)) {
    const int64_t idx = static_cast<int64_t>(hash % static_cast<uint64_t>(shard.bucket_cnt_));
    for (FtsShareEntry *cur = shard.buckets_[idx]; nullptr == entry && nullptr != cur;
         cur = cur->bucket_next_) {
      if (cur->hash_ == hash) {
        entry = cur;
      }
    }
  }
  return entry;
}

bool FtsTokenShareCache::matches(const FtsShareEntry &entry, const uint64_t parser_sig,
                                 const int32_t collation, const common::ObString &text)
{
  bool matched = false;
  if (entry.parser_sig_ == parser_sig
      && entry.collation_ == collation
      && entry.text_len_ == text.length()) {
    const int64_t len = static_cast<int64_t>(entry.text_len_);
    matched = (0 == len || 0 == MEMCMP(entry.text_, text.ptr(), len));
  }
  return matched;
}

void FtsTokenShareCache::lru_unlink(Shard &shard, FtsShareEntry &entry)
{
  if (nullptr != entry.older_) {
    entry.older_->newer_ = entry.newer_;
  } else {
    shard.oldest_ = entry.newer_;
  }
  if (nullptr != entry.newer_) {
    entry.newer_->older_ = entry.older_;
  } else {
    shard.newest_ = entry.older_;
  }
  entry.older_ = nullptr;
  entry.newer_ = nullptr;
}

void FtsTokenShareCache::lru_link_newest(Shard &shard, FtsShareEntry &entry)
{
  entry.older_ = shard.newest_;
  entry.newer_ = nullptr;
  if (nullptr != shard.newest_) {
    shard.newest_->newer_ = &entry;
  }
  shard.newest_ = &entry;
  if (nullptr == shard.oldest_) {
    shard.oldest_ = &entry;
  }
}

void FtsTokenShareCache::bucket_remove(Shard &shard, FtsShareEntry &entry)
{
  const int64_t idx = static_cast<int64_t>(entry.hash_ % static_cast<uint64_t>(shard.bucket_cnt_));
  if (nullptr != entry.bucket_prev_) {
    entry.bucket_prev_->bucket_next_ = entry.bucket_next_;
  } else {
    shard.buckets_[idx] = entry.bucket_next_;
  }
  if (nullptr != entry.bucket_next_) {
    entry.bucket_next_->bucket_prev_ = entry.bucket_prev_;
  }
  entry.bucket_next_ = nullptr;
  entry.bucket_prev_ = nullptr;
}

void FtsTokenShareCache::free_entry(FtsShareEntry *entry)
{
  if (nullptr != entry) {
    if (nullptr != entry->payload_) {
      ob_free(entry->payload_);
    }
    ob_free(entry);
  }
}

int FtsTokenShareCache::copy_out(common::ObIAllocator &allocator,
                                 const FtsShareEntry &entry, FtsSharedDoc &doc)
{
  int ret = OB_SUCCESS;
  const int64_t token_bytes = static_cast<int64_t>(sizeof(FtsSharedToken)) * entry.token_cnt_;
  const int64_t total = token_bytes + entry.word_bytes_;
  char *buf = nullptr;
  if (OB_UNLIKELY(entry.token_cnt_ <= 0 || nullptr == entry.payload_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = static_cast<char *>(allocator.alloc(total)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const FtsSharedToken *src = reinterpret_cast<const FtsSharedToken *>(entry.payload_);
    const char *src_word = entry.payload_ + token_bytes + entry.text_len_;
    FtsSharedToken *dst = reinterpret_cast<FtsSharedToken *>(buf);
    char *dst_word = buf + token_bytes;
    for (int64_t i = 0; i < entry.token_cnt_; ++i) {
      dst[i].len_ = src[i].len_;
      dst[i].cnt_ = src[i].cnt_;
      dst[i].reserved_ = 0;
      dst[i].ptr_ = dst_word;
      if (src[i].len_ > 0) {
        MEMCPY(dst_word, src_word, src[i].len_);
        dst_word += src[i].len_;
      }
      src_word += src[i].len_;
    }
    doc.doc_length_ = entry.doc_length_;
    doc.token_cnt_ = entry.token_cnt_;
    doc.tokens_ = dst;
  }
  return ret;
}

void FtsTokenShareCache::evict_if_needed(Shard &shard)
{
  while (shard.bytes_ > shard.budget_ && nullptr != shard.oldest_) {
    FtsShareEntry *victim = shard.oldest_;
    bucket_remove(shard, *victim);
    lru_unlink(shard, *victim);
    shard.bytes_ -= victim->size_;
    evictions_.fetch_add(1, std::memory_order_relaxed);
    free_entry(victim);
  }
}

int FtsTokenShareCache::fetch(const uint64_t hash, const uint64_t parser_sig,
                              const int32_t collation, const common::ObString &text,
                              common::ObIAllocator &allocator, FtsSharedDoc &doc)
{
  int ret = ensure_inited();
  if (OB_SUCCESS == ret) {
    Shard &shard = shards_[hash % FTS_TOKEN_SHARE_SHARD_CNT];
    lib::ObLockGuard<lib::ObMutex> guard(shard.mutex_);
    FtsShareEntry *entry = find(shard, hash);
    if (OB_ISNULL(entry) || !matches(*entry, parser_sig, collation, text)) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      lru_unlink(shard, *entry);
      lru_link_newest(shard, *entry);
      ret = copy_out(allocator, *entry, doc);
    }
  }
  const int64_t lookups = lookups_.fetch_add(1, std::memory_order_relaxed) + 1;
  if (OB_SUCC(ret)) {
    hits_.fetch_add(1, std::memory_order_relaxed);
  }
  if (0 == lookups % FTS_TOKEN_SHARE_LOG_INTERVAL) {
    report();
  }
  return ret;
}

int FtsTokenShareCache::try_lead(const uint64_t hash, LeadResult &result)
{
  int ret = ensure_inited();
  result = UNTRACKED;
  if (OB_SUCC(ret)) {
    Shard &shard = shards_[hash % FTS_TOKEN_SHARE_SHARD_CNT];
    lib::ObLockGuard<lib::ObMutex> guard(shard.mutex_);
    const int64_t now = ObTimeUtility::current_time();
    InflightSlot *free_slot = nullptr;
    bool found = false;
    for (int64_t i = 0; !found && i < FTS_TOKEN_SHARE_INFLIGHT_SLOTS; ++i) {
      InflightSlot &slot = shard.inflight_[i];
      const bool stale = slot.ctime_us_ > 0
          && now - slot.ctime_us_ > FTS_TOKEN_SHARE_INFLIGHT_TTL_US;
      if (slot.ctime_us_ > 0 && !stale) {
        if (slot.hash_ == hash) {
          found = true;
        }
      } else if (nullptr == free_slot) {
        free_slot = &slot;
      }
    }
    if (found) {
      result = WAIT;
    } else if (nullptr != free_slot) {
      free_slot->hash_ = hash;
      free_slot->ctime_us_ = now;
      result = LEAD;
    } else {
      result = UNTRACKED;
    }
  }
  return ret;
}

void FtsTokenShareCache::release_lead(const uint64_t hash)
{
  if (OB_SUCCESS == ensure_inited()) {
    Shard &shard = shards_[hash % FTS_TOKEN_SHARE_SHARD_CNT];
    lib::ObLockGuard<lib::ObMutex> guard(shard.mutex_);
    for (int64_t i = 0; i < FTS_TOKEN_SHARE_INFLIGHT_SLOTS; ++i) {
      if (shard.inflight_[i].ctime_us_ > 0 && shard.inflight_[i].hash_ == hash) {
        shard.inflight_[i].hash_ = 0;
        shard.inflight_[i].ctime_us_ = 0;
      }
    }
  }
}

void FtsTokenShareCache::put(const uint64_t hash, const uint64_t parser_sig,
                             const int32_t collation, const common::ObString &text,
                             const int64_t doc_length, const storage::ObFTWordMap &word_map)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ensure_inited())
      || 0 == limit_bytes_
      || text.length() <= 0
      || text.length() > FTS_TOKEN_SHARE_MAX_TEXT_LEN
      || word_map.size() <= 0) {
    // cache disabled or nothing to cache
  } else {
    const int64_t token_cnt = word_map.size();
    int64_t word_bytes = 0;
    for (storage::ObFTWordMap::const_iterator iter = word_map.begin();
         OB_SUCC(ret) && iter != word_map.end(); ++iter) {
      const ObString word = iter->first.get_word().get_string();
      if (OB_UNLIKELY(word.length() < 0)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        word_bytes += word.length();
      }
    }
    Shard &shard = shards_[hash % FTS_TOKEN_SHARE_SHARD_CNT];
    const int64_t token_bytes = static_cast<int64_t>(sizeof(FtsSharedToken)) * token_cnt;
    const int64_t payload_size = token_bytes + text.length() + word_bytes;
    const int64_t entry_size = payload_size + static_cast<int64_t>(sizeof(FtsShareEntry));
    char *payload = nullptr;
    FtsShareEntry *entry = nullptr;
    if (OB_FAIL(ret) || entry_size > shard.budget_) {
      // too large for one shard budget, do not cache
    } else if (OB_ISNULL(payload = static_cast<char *>(
                   ob_malloc(payload_size, ObMemAttr("FtsTokShare"))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(entry = static_cast<FtsShareEntry *>(
                   ob_malloc(sizeof(FtsShareEntry), ObMemAttr("FtsTokShare"))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      FtsSharedToken *tokens = reinterpret_cast<FtsSharedToken *>(payload);
      char *text_buf = payload + token_bytes;
      char *word_buf = text_buf + text.length();
      MEMCPY(text_buf, text.ptr(), text.length());
      int64_t idx = 0;
      for (storage::ObFTWordMap::const_iterator iter = word_map.begin();
           OB_SUCC(ret) && iter != word_map.end(); ++iter, ++idx) {
        const ObString word = iter->first.get_word().get_string();
        tokens[idx].len_ = word.length();
        tokens[idx].cnt_ = iter->second;
        tokens[idx].reserved_ = 0;
        tokens[idx].ptr_ = word_buf;
        if (word.length() > 0) {
          MEMCPY(word_buf, word.ptr(), word.length());
          word_buf += word.length();
        }
      }
      if (OB_SUCC(ret)) {
        entry->hash_ = hash;
        entry->parser_sig_ = parser_sig;
        entry->collation_ = collation;
        entry->text_len_ = static_cast<int32_t>(text.length());
        entry->doc_length_ = doc_length;
        entry->token_cnt_ = token_cnt;
        entry->word_bytes_ = word_bytes;
        entry->size_ = entry_size;
        entry->payload_ = payload;
        entry->text_ = text_buf;
        bool inserted = false;
        {
          lib::ObLockGuard<lib::ObMutex> guard(shard.mutex_);
          if (nullptr == find(shard, hash)) {
            // A different text hashing to the same value is a miss, so no
            // replacement is needed; the entry is simply not cached.
            const int64_t bucket_idx = static_cast<int64_t>(
                hash % static_cast<uint64_t>(shard.bucket_cnt_));
            entry->bucket_next_ = shard.buckets_[bucket_idx];
            entry->bucket_prev_ = nullptr;
            if (nullptr != shard.buckets_[bucket_idx]) {
              shard.buckets_[bucket_idx]->bucket_prev_ = entry;
            }
            shard.buckets_[bucket_idx] = entry;
            lru_link_newest(shard, *entry);
            shard.bytes_ += entry_size;
            inserted = true;
            evict_if_needed(shard);
          }
        }
        if (!inserted) {
          ret = OB_ENTRY_EXIST;
        } else {
          stores_.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    if (OB_FAIL(ret)) {
      if (nullptr != payload) {
        ob_free(payload);
        payload = nullptr;
        if (nullptr != entry) {
          entry->payload_ = nullptr;
        }
      }
      if (nullptr != entry) {
        ob_free(entry);
        entry = nullptr;
      }
      if (OB_ENTRY_EXIST != ret) {
        failed_.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

void FtsTokenShareCache::report()
{
  int64_t bytes = 0;
  for (int64_t i = 0; i < FTS_TOKEN_SHARE_SHARD_CNT; ++i) {
    Shard &shard = shards_[i];
    lib::ObLockGuard<lib::ObMutex> guard(shard.mutex_);
    bytes += shard.bytes_;
  }
  LOG_INFO("FTS_TOKEN_SHARE_STAT", "lookups", lookups_.load(), "hits", hits_.load(),
           "stores", stores_.load(), "evictions", evictions_.load(),
           "failed", failed_.load(), "bytes", bytes);
}

inline uint64_t fts_token_share_hash(const common::ObString &text,
                                     const int32_t collation,
                                     const uint64_t parser_sig)
{
  return common::murmurhash64A(text.ptr(),
                               static_cast<int32_t>(text.length()),
                               FTS_TOKEN_SHARE_HASH_SEED
                                   ^ (static_cast<uint64_t>(collation) << 32)
                                   ^ parser_sig);
}

inline uint64_t fts_token_share_parser_sig(const common::ObString &parser_name,
                                           const common::ObString &parser_properties)
{
  uint64_t sig = common::murmurhash64A(parser_name.ptr(),
                                       static_cast<int32_t>(parser_name.length()),
                                       FTS_TOKEN_SHARE_HASH_SEED);
  sig = common::murmurhash64A(parser_properties.ptr(),
                              static_cast<int32_t>(parser_properties.length()),
                              sig);
  return sig;
}

bool fts_token_share_enabled()
{
  return config::_enable_fts_token_share()
      && config::_fts_token_share_mem_limit_mb() > 0;
}

// Make this thread's short sleeps precise; see FTS_TOKEN_SHARE_TIMER_SLACK_NS.
void fts_token_share_tune_timer_once()
{
#ifdef __linux__
  static thread_local bool tuned = false;
  if (OB_UNLIKELY(!tuned)) {
    (void)prctl(PR_SET_TIMERSLACK, static_cast<unsigned long>(FTS_TOKEN_SHARE_TIMER_SLACK_NS),
                0, 0, 0);
    tuned = true;
  }
#endif
}

int fill_fulltext_word_rows_from_shared(common::ObIAllocator &allocator,
                                        const ObDatum &doc_id_datum,
                                        const bool is_fts_index_aux,
                                        const FtsSharedDoc &doc,
                                        ObDomainIndexRow &word_rows)
{
  int ret = OB_SUCCESS;
  static constexpr int64_t FT_WORD_DOC_COL_CNT = 4;
  void *rows_buf = nullptr;
  blocksstable::ObDatumRow *rows = nullptr;
  if (OB_UNLIKELY(doc.token_cnt_ <= 0 || nullptr == doc.tokens_)) {
    ret = OB_ITER_END;
  } else if (OB_ISNULL(rows_buf = static_cast<char *>(
                 allocator.alloc(doc.token_cnt_ * sizeof(blocksstable::ObDatumRow))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    const int64_t word_idx = is_fts_index_aux ? 0 : 1;
    const int64_t doc_id_idx = is_fts_index_aux ? 1 : 0;
    rows = new (rows_buf) blocksstable::ObDatumRow[doc.token_cnt_];
    for (int64_t i = 0; OB_SUCC(ret) && i < doc.token_cnt_; ++i) {
      const FtsSharedToken &token = doc.tokens_[i];
      if (OB_FAIL(rows[i].init(allocator, FT_WORD_DOC_COL_CNT))) {
      } else {
        rows[i].storage_datums_[word_idx].set_string(token.ptr_, static_cast<uint32_t>(token.len_));
        rows[i].storage_datums_[doc_id_idx].shallow_copy_from_datum(doc_id_datum);
        rows[i].storage_datums_[2].set_uint(static_cast<uint64_t>(token.cnt_));
        rows[i].storage_datums_[3].set_uint(static_cast<uint64_t>(doc.doc_length_));
        if (OB_FAIL(word_rows.push_back(&rows[i]))) {
        }
      }
    }
  }
  return ret;
}

// Diagnostic only (FTS_TOKEN_SHARE_CHECK=1): re-segments the document and compares
// the result with the rows just built from the shared cache, so a normal index
// build doubles as an equivalence test.  It never changes the build result.
bool fts_token_share_check_enabled()
{
  static const bool enabled = (nullptr != ::getenv("FTS_TOKEN_SHARE_CHECK"));
  return enabled;
}

void check_fulltext_word_rows_from_shared(storage::ObFTParseHelper *helper,
                                          const common::ObObjMeta &ft_obj_meta,
                                          const common::ObString &fulltext,
                                          const bool is_fts_index_aux,
                                          const ObDomainIndexRow &shared_rows)
{
  static thread_local int64_t checked = 0;
  static thread_local int64_t mismatch = 0;
  int ret = OB_SUCCESS;
  storage::ObFTWordMap word_map;
  int64_t doc_length = 0;
  const int64_t bucket_cnt = MIN(MAX(fulltext.length() / 10, 2), 997);
  bool equal = true;
  if (OB_ISNULL(helper)) {
    return;
  } else if (OB_FAIL(word_map.create(bucket_cnt, ObMemAttr("FtsShareChk")))) {
  } else if (OB_FAIL(helper->segment(ft_obj_meta, fulltext.ptr(), fulltext.length(),
                                     doc_length, word_map, true /* clear_words */))) {
  } else if (word_map.size() != shared_rows.count()) {
    equal = false;
  } else {
    const int64_t word_idx = is_fts_index_aux ? 0 : 1;
    int64_t i = 0;
    for (storage::ObFTWordMap::const_iterator iter = word_map.begin();
         equal && iter != word_map.end(); ++iter, ++i) {
      const blocksstable::ObDatumRow *row = shared_rows.at(i);
      if (OB_ISNULL(row)) {
        equal = false;
      } else {
        const ObString word = iter->first.get_word().get_string();
        const ObString shared_word = row->storage_datums_[word_idx].get_string();
        equal = word.length() == shared_word.length()
            && (0 == word.length()
                || 0 == MEMCMP(word.ptr(), shared_word.ptr(), word.length()))
            && static_cast<uint64_t>(iter->second) == row->storage_datums_[2].get_uint()
            && static_cast<uint64_t>(doc_length) == row->storage_datums_[3].get_uint();
      }
    }
  }
  (void)word_map.destroy();
  ++checked;
  if (!equal) {
    ++mismatch;
    LOG_ERROR("FTS token share check mismatch", K(ret), K(checked), K(mismatch),
              K(fulltext.length()), K(is_fts_index_aux));
  } else if (0 == checked % 1000) {
    LOG_INFO("FTS_TOKEN_SHARE_CHECK", K(checked), K(mismatch));
  }
}

} // namespace

ObFTIndexRowCache::ObFTIndexRowCache()
  : rows_(),
    row_idx_(0),
    is_fts_index_aux_(true),
    helper_(),
    word_map_(),
    parser_sig_(0),
    is_inited_(false)
{
}

ObFTIndexRowCache::~ObFTIndexRowCache()
{
  reset();
}

int ObFTIndexRowCache::init(
    const bool is_fts_index_aux,
    const common::ObString &parser_name,
    const common::ObString &parser_properties)
{
  int ret = OB_SUCCESS;
  lib::ContextParam param;
  param.set_mem_attr("DocIdMerge", ObCtxIds::DEFAULT_CTX_ID).set_properties(lib::USE_TL_PAGE_OPTIONAL);
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(CURRENT_CONTEXT->CREATE_CONTEXT(merge_memctx_, param))) {
  } else if (OB_FAIL(helper_.init(&(merge_memctx_->get_arena_allocator()), parser_name, parser_properties))) {
  } else {
    row_idx_ = 0;
    is_fts_index_aux_ = is_fts_index_aux;
    parser_sig_ = fts_token_share_parser_sig(parser_name, parser_properties);
    is_inited_ = true;
  }
  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObFTIndexRowCache::segment(const common::ObObjMeta &ft_obj_meta,
                               const ObDatum &doc_id_datum,
                               const ObString &fulltext)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(reuse())) {
  } else {
    // The two tokenized aux tables of one fulltext index are built by two
    // concurrent DDL sub tasks that tokenize the very same document text.  Share
    // the first result with the second one; see the cache comment above.
    const bool share = fts_token_share_enabled();
    const bool cacheable = share
        && fulltext.length() > 0
        && fulltext.length() <= FTS_TOKEN_SHARE_MAX_TEXT_LEN;
    const int32_t collation = static_cast<int32_t>(ft_obj_meta.get_collation_type());
    const uint64_t hash = cacheable
        ? fts_token_share_hash(fulltext, collation, parser_sig_) : 0;
    common::ObIAllocator &allocator = merge_memctx_->get_arena_allocator();
    FtsTokenShareCache &share_cache = FtsTokenShareCache::instance();
    static thread_local int64_t consecutive_waits = 0;
    bool from_share = false;
    bool is_leader = false;
    if (cacheable) {
      for (int64_t attempt = 0; !from_share && attempt <= FTS_TOKEN_SHARE_WAIT_RETRY; ++attempt) {
        FtsSharedDoc shared_doc;
        const int fetch_ret = share_cache.fetch(hash, parser_sig_, collation, fulltext,
                                                allocator, shared_doc);
        if (OB_SUCCESS == fetch_ret) {
          const int fill_ret = fill_fulltext_word_rows_from_shared(allocator, doc_id_datum,
                                                                   is_fts_index_aux_,
                                                                   shared_doc, rows_);
          if (OB_SUCC(fill_ret)) {
            from_share = true;
            consecutive_waits = 0;
            if (OB_UNLIKELY(fts_token_share_check_enabled())) {
              check_fulltext_word_rows_from_shared(&helper_, ft_obj_meta, fulltext,
                                                   is_fts_index_aux_, rows_);
            }
          } else if (OB_ITER_END != fill_ret) {
            LOG_WARN("failed to build fts rows from shared tokens, fallback to segment",
                     K(fill_ret), K(fulltext.length()));
          }
          break;
        }
        if (attempt == FTS_TOKEN_SHARE_WAIT_RETRY) {
          break;
        }
        FtsTokenShareCache::LeadResult lead_result = FtsTokenShareCache::UNTRACKED;
        if (OB_FAIL(share_cache.try_lead(hash, lead_result))) {
          break;
        } else if (FtsTokenShareCache::LEAD == lead_result) {
          is_leader = true;
          break;
        } else if (FtsTokenShareCache::UNTRACKED == lead_result) {
          // no coordination slot available: just do the work
          break;
        } else if (consecutive_waits >= FTS_TOKEN_SHARE_MAX_CONSECUTIVE_WAITS) {
          // waiting has not paid off recently, just do the work
          break;
        } else {
          // WAIT: another thread is tokenizing this very text.  Wait for its
          // result instead of duplicating the work; the wait is bounded so a
          // stalled leader can only delay this document, never the build.
          ++consecutive_waits;
          fts_token_share_tune_timer_once();
          if (attempt < 2) {
            sched_yield();
          } else {
            ::usleep(MIN(2 * attempt, FTS_TOKEN_SHARE_WAIT_MAX_SLEEP_US));
          }
        }
      }
    }
    if (from_share) {
      // rows_ already filled by the shared cache
    } else if (OB_FAIL(ObDASDomainUtils::generate_fulltext_word_rows(allocator,
                                                                     &helper_,
                                                                     ft_obj_meta,
                                                                     doc_id_datum,
                                                                     fulltext,
                                                                     is_fts_index_aux_,
                                                                     rows_,
                                                                     &word_map_,
                                                                     true))) {
    } else if (cacheable && rows_.count() > 0 && nullptr != rows_.at(0)) {
      // Publish this tokenization for the other aux table build.
      share_cache.put(hash, parser_sig_, collation, fulltext,
          static_cast<int64_t>(rows_.at(0)->storage_datums_[3].get_uint()),
          word_map_);
    }
    if (is_leader) {
      share_cache.release_lead(hash);
    }
    if (OB_SUCC(ret)) {
      row_idx_ = 0;
    }
  }
  LOG_TRACE("word segment", K(ret), K(row_idx_), K(rows_.count()), K(doc_id_datum), K(fulltext));
  return ret;
}

int ObFTIndexRowCache::get_next_row(blocksstable::ObDatumRow *&row)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (row_idx_ >= rows_.count()) {
    ret = OB_ITER_END;
  } else {
    row = (rows_[row_idx_]);
    ++row_idx_;
  }
  LOG_TRACE("get next row", K(ret), KPC(row), K(row_idx_), K(rows_.count()));
  return ret;
}

void ObFTIndexRowCache::reset()
{
  rows_.reset();
  row_idx_ = 0;
  is_fts_index_aux_ = true;
  helper_.reset();
  if (word_map_.created()) {
    (void)word_map_.destroy();
  }
  if (OB_NOT_NULL(merge_memctx_)) {
    DESTROY_CONTEXT(merge_memctx_);
    merge_memctx_ = nullptr;
  }
  is_inited_ = false;
}

int ObFTIndexRowCache::reuse()
{
  int ret = OB_SUCCESS;
  rows_.reuse();
  row_idx_ = 0;
  if (word_map_.created() && OB_FAIL(word_map_.reuse())) {
  } else if (OB_NOT_NULL(merge_memctx_)) {
    merge_memctx_->reset_remain_one_page();
  }
  return ret;
}

int ObDASDomainUtils::generate_spatial_index_rows(
    ObIAllocator &allocator,
    common::ObISrsProvider &srs_provider,
    const ObDASDMLBaseCtDef &das_ctdef,
    const ObString &wkb_str,
    const IntFixedArray &row_projector,
    const ObDASWriteBuffer::DmlRow &dml_row,
    ObDomainIndexRow &spat_rows)
{
  int ret = OB_SUCCESS;
  common::ObSrsCacheGuard srs_guard;
  const ObSrsItem *srs_item = NULL;
  const ObSrsBoundsItem *srs_bound = NULL;
  uint32_t srid = UINT32_MAX;
  uint64_t rowkey_num = das_ctdef.table_param_.get_data_table().get_rowkey_column_num();
  lib::ObMallocHookAttrGuard malloc_guard(lib::ObMemAttr("S2Adapter"));

  if (OB_FAIL(ObGeoTypeUtil::get_srid_from_wkb(wkb_str, srid))) {
  } else if (srid != 0 &&
      OB_FAIL(srs_provider.get_tenant_srs_guard(srs_guard))) {
  } else if (srid != 0 &&
      OB_FAIL(srs_guard.get_srs_item(srid, srs_item))) {
  } else if (((srid == 0) || !(srs_item->is_geographical_srs())) &&
              OB_FAIL(srs_provider.get_srs_bounds(srid, srs_item, srs_bound))) {
  } else {
    ObS2Adapter s2object(&allocator, srid != 0 ? srs_item->is_geographical_srs() : false);
    ObSpatialMBR spa_mbr;
    ObS2Cellids cellids;
    char *mbr = nullptr;
    int64_t mbr_len = 0;
    void *rows_buf = nullptr;
    blocksstable::ObDatumRow *rows = nullptr;
    if (OB_FAIL(s2object.init(wkb_str, srs_bound))) {
    } else if (OB_FAIL(s2object.get_cellids(cellids, false))) {
    } else if (OB_FAIL(s2object.get_mbr(spa_mbr))) {
    } else if (spa_mbr.is_empty()) {
      if (cellids.size() == 0) {
      } else {
        ret = OB_ERR_GIS_INVALID_DATA;
      }
    } else if (OB_ISNULL(mbr = reinterpret_cast<char *>(allocator.alloc(OB_DEFAULT_MBR_SIZE)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(spa_mbr.to_char(mbr, mbr_len))) {
    } else if (OB_ISNULL(rows_buf = reinterpret_cast<char *>(allocator.alloc(cellids.size() * sizeof(blocksstable::ObDatumRow))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      rows = new (rows_buf) blocksstable::ObDatumRow[cellids.size()];
      int64_t cellid_col_idx = 0;
      int64_t mbr_col_idx = 1;
      for (uint64_t i = 0; OB_SUCC(ret) && i < cellids.size(); i++) {
        if (OB_FAIL(rows[i].init(allocator, rowkey_num))) {
        } else {
          // Index row[cellid_obj][mbr_obj][rowkey_obj]
          for(uint64_t j = 0; OB_SUCC(ret) && j < rowkey_num; j++) {
            const ObObjMeta &col_type = das_ctdef.column_types_.at(j);
            const ObAccuracy &col_accuracy = das_ctdef.column_accuracys_.at(j);
            int64_t projector_idx = row_projector.at(j);
            if (FALSE_IT(rows[i].storage_datums_[j].shallow_copy_from_datum(dml_row.cells()[projector_idx]))) {
            } else if (rows[i].storage_datums_[j].is_null()) {
            } else if (OB_FAIL(data_plane::ObDatumReshape::reshape_datum_value(col_type, col_accuracy, allocator, rows[i].storage_datums_[j]))) {
            }
          }
          if (OB_SUCC(ret)) {
            rows[i].storage_datums_[cellid_col_idx].set_uint(cellids.at(i));
            ObString mbr_val(mbr_len, mbr);
            rows[i].storage_datums_[mbr_col_idx].set_string(mbr_val);
            // not set_collation_type(CS_TYPE_BINARY) and set_collation_level(CS_LEVEL_IMPLICIT)
            if (OB_FAIL(spat_rows.push_back(&rows[i]))) {
            }
          }
        }
      }
    }
  }

  return ret;
}

int ObDASDomainUtils::build_ft_doc_word_infos(
    const transaction::ObTxDesc *trans_desc,
    const transaction::ObTxReadSnapshot *snapshot,
    const common::ObIArray<const ObDASBaseCtDef *> &related_ctdefs,
    const common::ObIArray<common::ObTabletID> &related_tablet_ids,
    const bool is_main_table_in_fts_ddl,
    common::ObIArray<ObFTDocWordInfo> &doc_word_infos)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_ISNULL(snapshot))) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < related_ctdefs.count(); ++i) {
    ObFTDocWordInfo doc_word_info;
    const ObDASDMLBaseCtDef *related_ctdef = static_cast<const ObDASDMLBaseCtDef *>(related_ctdefs.at(i));
    if (OB_ISNULL(related_ctdef)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (related_ctdef->table_param_.get_data_table().is_fts_doc_word_aux()) {
      doc_word_info.table_id_ = related_ctdef->table_param_.get_data_table().get_table_id();
      doc_word_info.doc_word_table_id_ = related_ctdef->table_param_.get_data_table().get_table_id();
      doc_word_info.doc_word_tablet_id_ = related_tablet_ids.at(i);
      doc_word_info.doc_word_schema_version_ = related_ctdef->table_param_.get_data_table().get_schema_version();
      doc_word_info.doc_word_found_ = true;
    } else if (related_ctdef->table_param_.get_data_table().is_fts_index_aux()) {
      doc_word_info.table_id_ = related_ctdef->table_param_.get_data_table().get_table_id();
      doc_word_info.doc_word_found_ = false;
      int nwrite = 0;
      const common::ObString &index_name = related_ctdef->table_param_.get_data_table().get_index_name();
      const int64_t buf_size = OB_MAX_TABLE_NAME_BUF_LENGTH;
      char buf[buf_size] = {0};
      if (OB_FAIL(databuff_printf(buf, buf_size, "%.*s_fts_doc_word", index_name.length(), index_name.ptr()))) {
      }
      for (int64_t j = 0; OB_SUCC(ret) && !doc_word_info.doc_word_found_ && j < related_ctdefs.count(); ++j) {
        const ObDASDMLBaseCtDef *doc_word_related_ctdef = static_cast<const ObDASDMLBaseCtDef *>(related_ctdefs.at(j));
        if (OB_ISNULL(doc_word_related_ctdef)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (doc_word_related_ctdef->table_param_.get_data_table().is_fts_doc_word_aux()
            && 0 == doc_word_related_ctdef->table_param_.get_data_table().get_index_name().case_compare(buf)) {
          doc_word_info.doc_word_table_id_ = doc_word_related_ctdef->table_param_.get_data_table().get_table_id();
          doc_word_info.doc_word_tablet_id_ = related_tablet_ids.at(j);
          doc_word_info.doc_word_schema_version_ = doc_word_related_ctdef->table_param_.get_data_table().get_schema_version();
          doc_word_info.doc_word_found_ = true;
        }
      }
      if (OB_SUCC(ret) && !doc_word_info.doc_word_found_ && !is_main_table_in_fts_ddl) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (FAILEDx(doc_word_info.snapshot_.assign(*snapshot))) {
    } else if (OB_FAIL(doc_word_infos.push_back(doc_word_info))) {
    } else if (OB_FAIL(doc_word_infos.at(doc_word_infos.count()-1).snapshot_.refresh_seq_no(
                           data_plane::tx_desc_seq_base(trans_desc)))) {
    }
  }
  return ret;
}

/*static*/ int ObDASDomainUtils::generate_fulltext_word_rows(common::ObIAllocator &allocator,
                                                             storage::ObFTParseHelper *helper,
                                                             const common::ObObjMeta &ft_obj_meta,
                                                             const ObDatum &doc_id_datum,
                                                             const ObString &fulltext,
                                                             const bool is_fts_index_aux,
                                                             ObDomainIndexRow &word_rows,
                                                             storage::ObFTWordMap *reusable_word_map)
{
  return generate_fulltext_word_rows(allocator,
                                     helper,
                                     ft_obj_meta,
                                     doc_id_datum,
                                     fulltext,
                                     is_fts_index_aux,
                                     word_rows,
                                     reusable_word_map,
                                     false);
}

namespace
{

// Dual-writer milestone M1: one segmentation pass, two row layouts.
//   fts_index aux   : [WORD], [DOC_ID], [WORD_COUNT], [DOC_LENGTH]
//   fts_doc_word aux: [DOC_ID], [WORD], [WORD_COUNT], [DOC_LENGTH]
// The two layouts only differ by the word/doc_id swap, and the row at index i in
// both arrays describes the same word, so a reader can advance them in lockstep.
// The word strings are the parser tokens owned by the caller supplied word map /
// parse helper, so the caller must keep them alive while the rows are consumed
// (same contract as the single table version).
// TODO(dual-writer M2): promote to ObDASDomainUtils::generate_fulltext_word_rows_dual
// once the scan operator emits row pairs.
int generate_fulltext_word_rows_dual_impl(common::ObIAllocator &allocator,
                                          storage::ObFTParseHelper *helper,
                                          const common::ObObjMeta &ft_obj_meta,
                                          const ObDatum &doc_id_datum,
                                          const ObString &fulltext,
                                          ObDomainDMLIterator::ObDomainIndexRow &index_rows,
                                          ObDomainDMLIterator::ObDomainIndexRow &word_rows)
{
  int ret = OB_SUCCESS;
  static constexpr int64_t FT_WORD_DOC_COL_CNT = 4;
  static constexpr int64_t FT_MAX_WORD_BUCKET = 997;
  int64_t doc_length = 0;
  ObFTWordMap word_map;
  void *rows_buf = nullptr;
  blocksstable::ObDatumRow *rows = nullptr;
  const int64_t ft_word_bkt_cnt = MIN(MAX(fulltext.length() / 10, 2), FT_MAX_WORD_BUCKET);
  if (OB_ISNULL(helper) || OB_UNLIKELY(!ft_obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == fulltext.length()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(word_map.create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (OB_FAIL(helper->segment(ft_obj_meta,
                                     fulltext.ptr(),
                                     fulltext.length(),
                                     doc_length,
                                     word_map,
                                     true /* clear_words */))) {
  } else if (0 == word_map.size()) {
    ret = OB_ITER_END;
  } else {
    const int64_t one_doc_word_cnt = word_map.size();
    if (OB_ISNULL(rows_buf = static_cast<char *>(allocator.alloc(
            2 * one_doc_word_cnt * sizeof(blocksstable::ObDatumRow))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      // Note: like the single table version the words are emitted in word map
      // (hash bucket) order.  Re-sorting a document's words per row would only
      // help if the doc_id byte order matched the scan order, which it does not
      // (ObDocId::get_string() is a raw little endian {tablet_id, seq_id} pair).
      rows = new (rows_buf) blocksstable::ObDatumRow[2 * one_doc_word_cnt];
      int64_t i = 0;
      for (ObFTWordMap::const_iterator iter = word_map.begin();
           OB_SUCC(ret) && iter != word_map.end();
           ++iter, ++i) {
        blocksstable::ObDatumRow &index_row = rows[i];
        blocksstable::ObDatumRow &word_row = rows[one_doc_word_cnt + i];
        const ObFTWord &ft_word = iter->first;
        const int64_t word_cnt = iter->second;
        if (OB_FAIL(index_row.init(allocator, FT_WORD_DOC_COL_CNT))
            || OB_FAIL(word_row.init(allocator, FT_WORD_DOC_COL_CNT))) {
        } else {
          index_row.storage_datums_[0].set_datum(ft_word.get_word());
          index_row.storage_datums_[1].shallow_copy_from_datum(doc_id_datum);
          index_row.storage_datums_[2].set_uint(word_cnt);
          index_row.storage_datums_[3].set_uint(doc_length);
          word_row.storage_datums_[0].shallow_copy_from_datum(doc_id_datum);
          word_row.storage_datums_[1].set_datum(ft_word.get_word());
          word_row.storage_datums_[2].set_uint(word_cnt);
          word_row.storage_datums_[3].set_uint(doc_length);
          if (OB_FAIL(index_rows.push_back(&index_row))
              || OB_FAIL(word_rows.push_back(&word_row))) {
          }
        }
      }
    }
  }
  if (word_map.created()) {
    (void)word_map.destroy();
  }
  return ret;
}

// M1 self check (diagnostic only): enabled by FTS_DUAL_ROWS_CHECK=1. Re-runs the
// dual generation for the same document and compares both row sets against the
// rows the single table path just produced, so a normal index build doubles as
// an equivalence test. It never changes the result of the build.
bool fts_dual_rows_check_enabled()
{
  static const bool enabled = (nullptr != ::getenv("FTS_DUAL_ROWS_CHECK"));
  return enabled;
}

struct FtsDualCheckStat
{
  int64_t checked_;
  int64_t mismatch_;
  int64_t skipped_;
};

FtsDualCheckStat &fts_dual_check_stat()
{
  static thread_local FtsDualCheckStat stat = {0, 0, 0};
  return stat;
}

bool fts_datum_equal(const ObDatum &lhs,
                     const ObDatum &rhs,
                     const bool is_string)
{
  bool equal = false;
  if (lhs.is_null() || rhs.is_null()) {
    equal = lhs.is_null() && rhs.is_null();
  } else if (is_string) {
    const ObString lhs_str = lhs.get_string();
    const ObString rhs_str = rhs.get_string();
    equal = lhs_str.length() == rhs_str.length()
            && (0 == lhs_str.length()
                || 0 == MEMCMP(lhs_str.ptr(), rhs_str.ptr(), lhs_str.length()));
  } else {
    equal = lhs.get_uint() == rhs.get_uint();
  }
  return equal;
}

void check_fts_dual_rows(common::ObIAllocator &allocator,
                         storage::ObFTParseHelper *helper,
                         const common::ObObjMeta &ft_obj_meta,
                         const ObDatum &doc_id_datum,
                         const ObString &fulltext,
                         const bool is_fts_index_aux,
                         const ObDomainDMLIterator::ObDomainIndexRow &produced_rows)
{
  int ret = OB_SUCCESS;
  ObDomainDMLIterator::ObDomainIndexRow index_rows;
  ObDomainDMLIterator::ObDomainIndexRow word_rows;
  FtsDualCheckStat &stat = fts_dual_check_stat();
  ret = generate_fulltext_word_rows_dual_impl(allocator,
                                              helper,
                                              ft_obj_meta,
                                              doc_id_datum,
                                              fulltext,
                                              index_rows,
                                              word_rows);
  if (OB_ITER_END == ret) {
    ++stat.skipped_;
  } else if (OB_FAIL(ret)) {
    ++stat.mismatch_;
    LOG_WARN("fts dual rows check: dual generation failed", K(ret), K(fulltext.length()));
  } else if (index_rows.count() != produced_rows.count()
             || word_rows.count() != produced_rows.count()) {
    ++stat.mismatch_;
    LOG_WARN("fts dual rows check: row count mismatch", K(ret), K(index_rows.count()),
             K(word_rows.count()), "single_rows", produced_rows.count(), K(is_fts_index_aux));
  } else {
    // Both paths emit the same document's words in the same word map order, so the
    // i-th row of each array must describe the same word; compare every column.
    const int64_t word_idx = is_fts_index_aux ? 0 : 1;
    const int64_t doc_id_idx = is_fts_index_aux ? 1 : 0;
    bool equal = true;
    for (int64_t i = 0; OB_SUCC(ret) && equal && i < index_rows.count(); ++i) {
      const blocksstable::ObDatumRow *single = produced_rows.at(i);
      const blocksstable::ObDatumRow *dual_index = index_rows.at(i);
      const blocksstable::ObDatumRow *dual_word = word_rows.at(i);
      if (OB_ISNULL(single) || OB_ISNULL(dual_index) || OB_ISNULL(dual_word)) {
        equal = false;
      } else {
        equal = fts_datum_equal(single->storage_datums_[word_idx],
                                dual_index->storage_datums_[0], true /* string */)
             && fts_datum_equal(single->storage_datums_[doc_id_idx],
                                dual_index->storage_datums_[1], true /* string */)
             && fts_datum_equal(single->storage_datums_[2],
                                dual_index->storage_datums_[2], false /* uint */)
             && fts_datum_equal(single->storage_datums_[3],
                                dual_index->storage_datums_[3], false /* uint */)
             && fts_datum_equal(dual_index->storage_datums_[0],
                                dual_word->storage_datums_[1], true /* string */)
             && fts_datum_equal(dual_index->storage_datums_[1],
                                dual_word->storage_datums_[0], true /* string */)
             && fts_datum_equal(dual_index->storage_datums_[2],
                                dual_word->storage_datums_[2], false /* uint */)
             && fts_datum_equal(dual_index->storage_datums_[3],
                                dual_word->storage_datums_[3], false /* uint */);
      }
      if (!equal) {
        ++stat.mismatch_;
        LOG_WARN("fts dual rows check: datum mismatch", K(i), K(is_fts_index_aux),
                 KPC(single), KPC(dual_index), KPC(dual_word));
      }
    }
  }
  ++stat.checked_;
  if (0 == stat.checked_ % 1000 || stat.mismatch_ > 0) {
    LOG_INFO("FTS_DUAL_ROWS_CHECK", "checked", stat.checked_,
             "mismatch", stat.mismatch_, "skipped", stat.skipped_);
  }
}

} // namespace

/*static*/ int ObDASDomainUtils::generate_fulltext_word_rows(common::ObIAllocator &allocator,
                                                             storage::ObFTParseHelper *helper,
                                                             const common::ObObjMeta &ft_obj_meta,
                                                             const ObDatum &doc_id_datum,
                                                             const ObString &fulltext,
                                                             const bool is_fts_index_aux,
                                                             ObDomainIndexRow &word_rows,
                                                             storage::ObFTWordMap *reusable_word_map,
                                                             const bool reusable_word_map_already_cleared)
{
  int ret = OB_SUCCESS;
  static int64_t FT_WORD_DOC_COL_CNT = 4;
  static constexpr int64_t FT_MAX_WORD_BUCKET = 997;
  const int64_t ft_word_bkt_cnt = MIN(MAX(fulltext.length() / 10, 2), FT_MAX_WORD_BUCKET);
  int64_t doc_length = 0;
  ObFTWordMap local_word_map;
  ObFTWordMap *ft_word_map = (nullptr != reusable_word_map) ? reusable_word_map : &local_word_map;
  void *rows_buf = nullptr;
  blocksstable::ObDatumRow *rows = nullptr;
  if (nullptr != reusable_word_map
      && !reusable_word_map_already_cleared
      && reusable_word_map->created()
      && OB_FAIL(reusable_word_map->reuse())) {
  } else if (OB_ISNULL(helper) || OB_UNLIKELY(!ft_obj_meta.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == fulltext.length()) {
    ret = OB_ITER_END;
  } else if (!ft_word_map->created()
             && OB_FAIL(ft_word_map->create(ft_word_bkt_cnt, common::ObMemAttr("FTWordMap")))) {
  } else if (OB_FAIL(segment_and_calc_word_count(allocator,
                                                 helper,
                                                 ft_obj_meta,
                                                 fulltext,
                                                 doc_length,
                                                 *ft_word_map,
                                                 false))) {
  } else if (0 == ft_word_map->size()) {
    ret = OB_ITER_END;
  } else if (OB_ISNULL(rows_buf = reinterpret_cast<char *>(
                           allocator.alloc(ft_word_map->size() * sizeof(blocksstable::ObDatumRow))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    int64_t i = 0;
    rows = new (rows_buf) blocksstable::ObDatumRow[ft_word_map->size()];
    for (ObFTWordMap::const_iterator iter = ft_word_map->begin();
         OB_SUCC(ret) && iter != ft_word_map->end();
         ++iter) {
      if (OB_FAIL(rows[i].init(allocator, FT_WORD_DOC_COL_CNT))) {
      } else {
        const ObFTWord &ft_word = iter->first;
        const int64_t word_cnt = iter->second;
        // index row format
        //  -    FTS_INDEX: [WORD], [DOC_ID], [WORD_COUNT], [DOC_LENGTH]
        //  - FTS_DOC_WORD: [DOC_ID], [WORD], [WORD_COUNT], [DOC_LENGTH]
        const int64_t word_idx = is_fts_index_aux ? 0 : 1;
        const int64_t doc_id_idx = is_fts_index_aux ? 1 : 0;
        const int64_t word_cnt_idx = 2;
        const int64_t doc_len_idx = 3;
        rows[i].storage_datums_[word_idx].set_datum(ft_word.get_word());
        rows[i].storage_datums_[doc_id_idx].shallow_copy_from_datum(doc_id_datum);
        rows[i].storage_datums_[word_cnt_idx].set_uint(word_cnt);
        rows[i].storage_datums_[doc_len_idx].set_uint(doc_length);
        if (OB_FAIL(word_rows.push_back(&rows[i]))) {
        } else {
          ++i;
        }
      }
    }
  }
  if (nullptr == reusable_word_map && local_word_map.created()) {
    (void)local_word_map.destroy();
  }
  // M1 diagnostic: prove the dual generation matches this single table output.
  if (OB_UNLIKELY(fts_dual_rows_check_enabled()) && OB_SUCC(ret)) {
    check_fts_dual_rows(allocator,
                        helper,
                        ft_obj_meta,
                        doc_id_datum,
                        fulltext,
                        is_fts_index_aux,
                        word_rows);
  }
  return ret;
}

/*static*/ int ObDASDomainUtils::segment_and_calc_word_count(
    common::ObIAllocator &allocator,
    storage::ObFTParseHelper *helper,
    const common::ObObjMeta &meta,
    const ObString &fulltext,
    int64_t &doc_length,
    ObFTWordMap &words_count,
    const bool clear_words)
{
  int ret = OB_SUCCESS;
  ObCollationType type = meta.get_collation_type();
  if (OB_ISNULL(helper)
      || OB_UNLIKELY(ObCollationType::CS_TYPE_INVALID == type
                  || ObCollationType::CS_TYPE_PINYIN_BEGIN_MARK <= type)
      || OB_UNLIKELY(!words_count.created())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(helper->segment(meta,
                                     fulltext.ptr(),
                                     fulltext.length(),
                                     doc_length,
                                     words_count,
                                     clear_words))) {
  }

  STORAGE_FTS_LOG(TRACE, "segment and calc word count", K(ret), K(words_count.size()), K(type));
  return ret;
}

int ObDASDomainUtils::get_pure_mutivalue_data(const ObString &json_str, const char*& data, int64_t& data_len, uint32_t& record_num)
{
  int ret = OB_SUCCESS;

  ObJsonBin bin(json_str.ptr(), json_str.length());

  if (OB_FAIL(bin.reset_iter())) {
  } else if (!ObJsonVerType::is_opaque_or_string(bin.json_type())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    data = bin.get_data();
    data_len = bin.get_data_length();

    record_num = *reinterpret_cast<const uint32_t*>(data);
  }

  return ret;
}

int ObDASDomainUtils::calc_save_rowkey_policy(
    ObIAllocator &allocator,
    const ObDASDMLBaseCtDef &das_ctdef,
    const IntFixedArray &row_projector,
    const ObDASWriteBuffer::DmlRow &dml_row,
    const int64_t record_cnt,
    bool& is_save_rowkey)
{
  int ret = OB_SUCCESS;
  is_save_rowkey = true;

  uint64_t column_num = das_ctdef.column_ids_.count();
  const int64_t data_table_rowkey_cnt = das_ctdef.table_param_.get_data_table().get_data_table_rowkey_column_num();
  const uint64_t multivalue_arr_col_id = das_ctdef.table_param_.get_data_table().get_multivalue_array_col_id();

  uint64_t mulvalue_column_start = 0;

  // -1 : doc id column
  uint64_t mulvalue_column_end = column_num - 1 - data_table_rowkey_cnt;

  if (mulvalue_column_end <= mulvalue_column_start) {
    ret = OB_INVALID_ARGUMENT;
  } else {

    ObObj *obj_arr = nullptr;
    if (OB_ISNULL(obj_arr = reinterpret_cast<ObObj *>(allocator.alloc(sizeof(ObObj) * column_num)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }

    uint32_t pure_data_size = 0;
    uint64_t rowkey_column_start = mulvalue_column_end;
    // -1 :  doc id column
    uint64_t rowkey_column_end = column_num - 1;

    for(uint64_t j = rowkey_column_start; OB_SUCC(ret) && j < rowkey_column_end; j++) {
      obj_arr[j].set_nop_value();
      const ObObjMeta &col_type = das_ctdef.column_types_.at(j);
      const ObAccuracy &col_accuracy = das_ctdef.column_accuracys_.at(j);
      int64_t projector_idx = row_projector.at(j);
      if (das_ctdef.column_ids_.at(j) == multivalue_arr_col_id) {
         // do nothing
      } else if (OB_FAIL(dml_row.cells()[projector_idx].to_obj(obj_arr[j], col_type))) {
      } else {
        pure_data_size += obj_arr[j].get_serialize_size();
      }
    }

    if (OB_SUCC(ret)) {
      if (record_cnt < MVI_FULL_ROWKEY_THRESHOLD) {
        is_save_rowkey = true;
      } else if (pure_data_size > MVI_ROWKEY_SIZE_THRESHOLD) {
        is_save_rowkey = false;
      } else {
        is_save_rowkey = true;
      }
    }
  }


  return ret;
}

int ObDASDomainUtils::generate_multivalue_index_rows(ObIAllocator &allocator,
                                               const ObDASDMLBaseCtDef &das_ctdef,
                                               int64_t multivalue_idx,
                                               int64_t multivalue_arr_idx,
                                               const ObString &json_str,
                                               const IntFixedArray &row_projector,
                                               const ObDASWriteBuffer::DmlRow &dml_row,
                                               ObDomainIndexRow &mvi_rows)
{
  int ret = OB_SUCCESS;
  const char* data = nullptr;
  int64_t data_len = 0;
  uint32_t record_num = 0;
  bool is_unique_index = das_ctdef.table_param_.get_data_table().get_index_type() == ObIndexType::INDEX_TYPE_UNIQUE_MULTIVALUE_LOCAL;

  if (OB_FAIL(get_pure_mutivalue_data(json_str, data, data_len, record_num))) {
  } else if (record_num == 0 && is_unique_index) {
  } else {
    uint32_t real_record_num = (record_num == 0 && !is_unique_index) ? 1 : record_num;
    const int64_t data_table_rowkey_cnt = das_ctdef.table_param_.get_data_table().get_data_table_rowkey_column_num();
    uint64_t column_num = das_ctdef.column_ids_.count();
    // for normal multivalue index, column_ids: [multivalue_col, rowkeys, docid]
    // when doc id optimization is enabled, column_ids: [multivalue_col, pk_increment]
    bool use_docid = !(data_table_rowkey_cnt == 1 && column_num == 2);

    void *rows_buf = nullptr;
    bool is_save_rowkey = true;
    if (OB_ISNULL(rows_buf = reinterpret_cast<char *>(allocator.alloc(real_record_num * sizeof(blocksstable::ObDatumRow))))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (use_docid && OB_FAIL(calc_save_rowkey_policy(allocator, das_ctdef, row_projector, dml_row, record_num, is_save_rowkey))) {
    } else {
      uint64_t rowkey_column_start = column_num - 1 - data_table_rowkey_cnt; // only used when use_docid
      uint64_t rowkey_column_end = column_num - 1;  // only used when use_docid
      bool is_none_unique_done = false;
      int64_t pos = sizeof(uint32_t);
      blocksstable::ObDatumRow *rows = new (rows_buf) blocksstable::ObDatumRow[real_record_num];

      for (int i = 0; OB_SUCC(ret) && (i < record_num || !is_none_unique_done) ; ++i) {
        if (OB_FAIL(rows[i].init(allocator, column_num))) {
        } else {
          for(uint64_t j = 0; OB_SUCC(ret) && j < column_num; j++) {
            ObObjMeta col_type = das_ctdef.column_types_.at(j);
            const ObAccuracy &col_accuracy = das_ctdef.column_accuracys_.at(j);
            int64_t projector_idx = row_projector.at(j);

            if (multivalue_idx == projector_idx) {
              ObObj obj;
              obj.set_nop_value();
              if (OB_FAIL(obj.deserialize(data, data_len, pos))) {
              } else {
                is_none_unique_done = true;
                if (ob_is_number_or_decimal_int_tc(col_type.get_type()) || ob_is_temporal_type(col_type.get_type())) {
                  col_type.set_collation_level(CS_LEVEL_NUMERIC);
                } else {
                  col_type.set_collation_level(CS_LEVEL_IMPLICIT);
                }
                if (!obj.is_null()) {
                  obj.set_meta_type(col_type);
                }
                if (OB_FAIL(rows[i].storage_datums_[j].from_obj_enhance(obj))) {
                }
              }
            } else if (!is_save_rowkey && (j >= rowkey_column_start && j < rowkey_column_end)) {
              rows[i].storage_datums_[j].set_null();
            } else if (multivalue_arr_idx == projector_idx) {
              rows[i].storage_datums_[j].set_null();
            } else {
              rows[i].storage_datums_[j].shallow_copy_from_datum(dml_row.cells()[projector_idx]);
            }

            if (rows[i].storage_datums_[j].is_null()) {  // do nothing
            } else if (OB_SUCC(ret) && OB_FAIL(data_plane::ObDatumReshape::reshape_datum_value(col_type, col_accuracy, allocator, rows[i].storage_datums_[j]))) {
            }
          }

          if (OB_SUCC(ret)) {
            if (OB_FAIL(mvi_rows.push_back(&rows[i]))) {
            }
          } // end if (OB_SUCC(ret))
        }
      }
    }
  }

  return ret;
}

int ObDomainDMLIterator::create_domain_dml_iterator(
    const ObDomainDMLParam &param,
    ObDomainDMLIterator *&domain_iter)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(param.row_projector_) || OB_ISNULL(param.das_ctdef_)
      || OB_ISNULL(param.lob_read_options_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (param.das_ctdef_->table_param_.get_data_table().is_spatial_index()) {
    void *buf = nullptr;
    if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObSpatialDMLIterator)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      domain_iter = new (buf) ObSpatialDMLIterator(param.allocator_, param.row_projector_, param.write_iter_,
                                                   param.das_ctdef_, param.main_ctdef_, param.srs_provider_,
                                                   param.lob_read_options_);
    }
  } else if (param.das_ctdef_->table_param_.get_data_table().is_fts_index()) {
    void *buf = nullptr;
    if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObFTDMLIterator)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObFTDMLIterator *iter = new (buf) ObFTDMLIterator(param.mode_, param.ft_doc_word_info_, param.allocator_,
                                                        param.row_projector_, param.write_iter_, param.das_ctdef_,
                                                        param.main_ctdef_, param.lob_read_options_);
      if (OB_FAIL(iter->init(param.das_ctdef_->table_param_.get_data_table().get_fts_parser_name(),
                             param.das_ctdef_->table_param_.get_data_table().get_fts_parser_property()))) {
        iter->~ObFTDMLIterator();
      } else {
        domain_iter = static_cast<ObDomainDMLIterator *>(iter);
      }
    }
  } else if (param.das_ctdef_->table_param_.get_data_table().is_multivalue_index()) {
    void *buf = nullptr;
    if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObMultivalueDMLIterator)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObMultivalueDMLIterator *iter = new (buf) ObMultivalueDMLIterator(param.allocator_, param.row_projector_,
                                                                        param.write_iter_, param.das_ctdef_,
                                                                        param.main_ctdef_, param.lob_read_options_);
      domain_iter = static_cast<ObDomainDMLIterator *>(iter);
    }
  } else if (param.das_ctdef_->table_param_.get_data_table().is_vector_index()) {
    if (param.das_ctdef_->table_param_.get_data_table().is_sparse_vector_index()) {
      void *buf = nullptr;
      if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObSparseVecIndexDMLIterator)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObSparseVecIndexDMLIterator *iter = new (buf) ObSparseVecIndexDMLIterator(param.allocator_, param.row_projector_,
                                                                                  param.write_iter_, param.das_ctdef_,
                                                                                  param.main_ctdef_, param.lob_read_options_);
        domain_iter = static_cast<ObDomainDMLIterator *>(iter);
      }
    } else if (share::schema::is_hybrid_vec_index_log_type(param.das_ctdef_->table_param_.get_data_table().get_index_type())) {
      void *buf = nullptr;
      if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObHybridVecLogDMLIterator)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObHybridVecLogDMLIterator *iter = new (buf) ObHybridVecLogDMLIterator(param.allocator_, param.row_projector_,
                                                                              param.write_iter_, param.das_ctdef_,
                                                                              param.main_ctdef_, param.lob_read_options_);
        domain_iter = static_cast<ObDomainDMLIterator *>(iter);
      }
    } else if (share::schema::is_hybrid_vec_index_embedded_type(param.das_ctdef_->table_param_.get_data_table().get_index_type())) {
      void *buf = nullptr;
      if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObEmbeddedVecDMLIterator)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObEmbeddedVecDMLIterator *iter = new (buf) ObEmbeddedVecDMLIterator(param.allocator_, param.row_projector_,
                                                                            param.write_iter_, param.das_ctdef_,
                                                                            param.main_ctdef_, param.lob_read_options_);
        domain_iter = static_cast<ObDomainDMLIterator *>(iter);
      }
    } else {
      void *buf = nullptr;
      if (OB_ISNULL(buf = param.allocator_.alloc(sizeof(ObVecIndexDMLIterator)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        ObVecIndexDMLIterator *iter = new (buf) ObVecIndexDMLIterator(param.allocator_, param.row_projector_,
                                                                      param.write_iter_, param.das_ctdef_,
                                                                      param.main_ctdef_, param.lob_read_options_);
        domain_iter = static_cast<ObDomainDMLIterator *>(iter);
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

ObDomainDMLIterator::ObDomainDMLIterator(
    common::ObIAllocator &allocator,
    const IntFixedArray *row_projector,
    ObDASWriteBuffer::Iterator &write_iter,
    const ObDASDMLBaseCtDef *das_ctdef,
    const ObDASDMLBaseCtDef *main_ctdef,
    const common::ObLobReadOptions *lob_read_options)
  : mode_(ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT),
    row_idx_(0),
    rows_(),
    row_projector_(row_projector),
    write_iter_(write_iter),
    das_ctdef_(das_ctdef),
    main_ctdef_(main_ctdef),
    allocator_(allocator),
    is_update_(nullptr == main_ctdef),
    lob_read_options_(lob_read_options)
{
}

ObDomainDMLIterator::~ObDomainDMLIterator()
{
  reset();
}

void ObDomainDMLIterator::reset()
{
  mode_ = ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT;
  row_idx_ = 0;
  rows_.reset();
  row_projector_ = nullptr;
  das_ctdef_ = nullptr;
  main_ctdef_ = nullptr;
  allocator_.reset();
}

void ObDomainDMLIterator::set_ctdef(
    const ObDASDMLBaseCtDef *das_ctdef,
    const IntFixedArray *row_projector,
    const ObDomainDMLMode &mode)
{
  mode_ = mode;
  row_idx_ = 0;
  das_ctdef_ = das_ctdef;
  row_projector_ = row_projector;
}

bool ObDomainDMLIterator::is_same_domain_type(const ObDASDMLBaseCtDef *das_ctdef) const
{
  bool is_same_domain_type = false;
  if (OB_NOT_NULL(das_ctdef) && OB_NOT_NULL(das_ctdef_)) {
    const data_plane::ObDmlTableView table_param = das_ctdef->table_param_.get_data_table();
    const data_plane::ObDmlTableView my_table_param = das_ctdef_->table_param_.get_data_table();
    if ((table_param.is_fts_index() && my_table_param.is_fts_index())
        || (table_param.is_multivalue_index() && my_table_param.is_multivalue_index())) {
      is_same_domain_type = true;
    }
  }
  return is_same_domain_type;
}

int ObDomainDMLIterator::change_domain_dml_mode(const ObDomainDMLMode &mode)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(mode < ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT || mode >= ObDomainDMLMode::DOMAIN_DML_MODE_MAX)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    mode_ = mode;
  }
  return ret;
}

int ObDomainDMLIterator::get_next_domain_row(blocksstable::ObDatumRow *&row)
{
  int ret = OB_SUCCESS;
  const ObChunkDatumStore::StoredRow *sr = nullptr;
  bool got_row = false;
  if (OB_FAIL(THIS_WORKER.check_status())) {
  }
  while (OB_SUCC(ret) && !got_row) {
    if (row_idx_ >= rows_.count()) {
      rows_.reuse();
      row_idx_ = 0;
      if (OB_FAIL(reuse_domain_row_state())) {
      } else if (FALSE_IT(allocator_.reuse())) {
      } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_domain_index())) {
        ret = OB_ERR_UNEXPECTED;
      } else if (FAILEDx(write_iter_.get_next_row(sr))) {
        if (OB_ITER_END != ret) {
        }
      } else if (OB_FAIL(generate_domain_rows(sr))) {
        if (ret != OB_ITER_END) {
        } else {
          ret = OB_SUCCESS;
        }
      }
    }
    if (OB_SUCC(ret) && row_idx_ < rows_.count()) {
      row = rows_[row_idx_];
      ++row_idx_;
      got_row = true;
    }
  }
  return ret;
}

int ObDomainDMLIterator::get_next_domain_rows(blocksstable::ObDatumRow *&row, int64_t &row_count)
{
  int ret = OB_SUCCESS;
  const ObChunkDatumStore::StoredRow *sr = nullptr;
  bool got_row = false;
  if (!das_ctdef_->table_param_.get_data_table().is_fts_index()) { // batch only for fulltext
    if (OB_FAIL(get_next_domain_row(row))) {
    } else {
      row_count = 1;
    }
  } else if (OB_FAIL(THIS_WORKER.check_status())) {
  } else {
    row_count = 0;
    while (OB_SUCC(ret) && !got_row) {
      if (row_idx_ >= rows_.count()) {
        rows_.reuse();
        row_idx_ = 0;
        if (OB_FAIL(reuse_domain_row_state())) {
        } else if (FALSE_IT(allocator_.reuse())) {
        } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_domain_index())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (FAILEDx(write_iter_.get_next_row(sr))) {
          if (OB_ITER_END != ret) {
          }
        } else if (OB_FAIL(generate_domain_rows(sr))) {
          if (ret != OB_ITER_END) {
          } else {
            ret = OB_SUCCESS;
          }
        }
      }
      if (OB_SUCC(ret) && row_idx_ < rows_.count()) {
        row = rows_[row_idx_];
        row_count = rows_.count() - row_idx_;
        row_idx_ = rows_.count();
        got_row = true;
      }
    }
  }
  return ret;
}

int ObSpatialDMLIterator::generate_domain_rows(const ObChunkDatumStore::StoredRow *store_row)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(store_row)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_spatial_index())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const uint64_t geo_col_id = das_ctdef_->table_param_.get_data_table().get_spatial_geo_col_id();
    int64_t geo_idx = -1;
    ObString geo_wkb;
    ObObjMeta geo_meta;
    if (!is_update_ && OB_FAIL(get_geo_wkb(store_row, geo_idx, geo_wkb, geo_meta))) {
    } else if (is_update_ && OB_FAIL(get_geo_wkb_for_update(store_row, geo_idx, geo_wkb, geo_meta))) {
    } else if (OB_UNLIKELY(geo_idx == -1)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                       *lob_read_options_, &allocator_, geo_meta.get_type(),
                       geo_meta.get_collation_type(), is_update_ ? true : geo_meta.has_lob_header(), geo_wkb))) {
    } else if (OB_ISNULL(srs_provider_)) {
      ret = OB_NOT_INIT;
    } else if (OB_FAIL(ObDASDomainUtils::generate_spatial_index_rows(
                  allocator_, *srs_provider_, *das_ctdef_, geo_wkb,
                                                              *row_projector_, *store_row, rows_))) {
    }
  }
  return ret;
}

int ObSpatialDMLIterator::get_geo_wkb(
    const ObChunkDatumStore::StoredRow *store_row,
    int64_t &geo_idx,
    ObString &geo_wkb,
    ObObjMeta &geo_meta) const
{
  int ret = OB_SUCCESS;
  const uint64_t rowkey_num = das_ctdef_->table_param_.get_data_table().get_rowkey_column_num();
  geo_idx = row_projector_->at(rowkey_num);
  if (geo_idx >= store_row->cnt_) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    geo_wkb = store_row->cells()[geo_idx].get_string();
    bool found = false;
    uint64_t geo_col_id = das_ctdef_->table_param_.get_data_table().get_spatial_geo_col_id();
    for (int64_t i = 0; OB_SUCC(ret) && i < main_ctdef_->column_ids_.count() && !found; ++i) {
      if (geo_col_id == main_ctdef_->column_ids_.at(i)) {
        geo_meta = main_ctdef_->column_types_.at(i);
        found = true;
      }
    }
  }
  return ret;
}

int ObSpatialDMLIterator::get_geo_wkb_for_update(
    const ObChunkDatumStore::StoredRow *store_row,
    int64_t &geo_idx,
    ObString &geo_wkb,
    ObObjMeta &geo_meta) const
{
  int ret = OB_SUCCESS;
  const uint64_t rowkey_num = das_ctdef_->table_param_.get_data_table().get_rowkey_column_num();
  const uint64_t old_proj = das_ctdef_->old_row_projector_.count();
  const uint64_t new_proj = das_ctdef_->new_row_projector_.count();
  geo_idx = -1;
  if (OB_UNLIKELY(rowkey_num + 1 != old_proj || rowkey_num + 1 != new_proj)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // get full row successfully
    geo_idx = row_projector_->at(rowkey_num);
    geo_wkb = store_row->cells()[geo_idx].get_string();
    geo_meta.set_type(ObGeometryType);
    geo_meta.set_collation_type(CS_TYPE_UTF8MB4_BIN);
  }
  return ret;
}

void ObFTDMLIterator::reset()
{
  is_inited_ = false;
  data_plane::reset_ft_doc_word_iterator(ft_doc_word_iter_);
  ft_parse_helper_.reset();
  if (word_map_.created()) {
    (void)word_map_.destroy();
  }
  ObDomainDMLIterator::reset();
}

int ObFTDMLIterator::reuse_domain_row_state()
{
  int ret = OB_SUCCESS;
  if (word_map_.created() && OB_FAIL(word_map_.reuse())) {
  }
  return ret;
}

ObFTDMLIterator::~ObFTDMLIterator()
{
  reset();
  data_plane::destroy_ft_doc_word_iterator(ft_doc_word_iter_);
}

int ObFTDMLIterator::rewind()
{
  int ret = OB_SUCCESS;
  row_idx_ = 0;
  if (das_ctdef_->table_param_.get_data_table().is_fts_index()) {
    if (OB_FAIL(ObDomainDMLIterator::rewind())) {
    } else if (OB_ISNULL(das_ctdef_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT == mode_) {
      const common::ObString &parser_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_name();
      const common::ObString &parser_property_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_property();
      bool is_same = false;
      if (OB_FAIL(ft_parse_helper_.check_is_the_same(parser_str, parser_property_str, is_same))) {
      } else if (is_same) {
        // This is the same as the parser name of the previous index.
        // nothing to do, just skip.
      } else if (FALSE_IT(ft_parse_helper_.reset())) {
      } else if (OB_FAIL(ft_parse_helper_.init(&allocator_, parser_str, parser_property_str))) {
      }
    } else if (ObDomainDMLMode::DOMAIN_DML_MODE_FT_SCAN == mode_) {
      if (OB_ISNULL(ft_doc_word_iter_)
          && OB_FAIL(data_plane::create_ft_doc_word_iterator(
                 ft_doc_word_allocator_, ft_doc_word_iter_))) {
      } else if (OB_ISNULL(doc_word_info_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (FALSE_IT(data_plane::reset_ft_doc_word_iterator(ft_doc_word_iter_))) {
      } else if (OB_FAIL(data_plane::init_ft_doc_word_iterator(
                             ft_doc_word_iter_,
                             doc_word_info_->doc_word_table_id_,
                             doc_word_info_->doc_word_tablet_id_,
                             &doc_word_info_->snapshot_,
                             doc_word_info_->doc_word_schema_version_))) {
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObFTDMLIterator::init(
    const common::ObString &parser_name,
    const common::ObString &parser_properties)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(ft_doc_word_iter_)
             && OB_FAIL(data_plane::create_ft_doc_word_iterator(
                    ft_doc_word_allocator_, ft_doc_word_iter_))) {
  } else {
    switch (mode_) {
      case ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT: {
        if (OB_FAIL(ft_parse_helper_.init(&allocator_, parser_name, parser_properties))) {
        }
        break;
      }
      case ObDomainDMLMode::DOMAIN_DML_MODE_FT_SCAN: {
        if (OB_ISNULL(ft_doc_word_iter_)
            && OB_FAIL(data_plane::create_ft_doc_word_iterator(
                   ft_doc_word_allocator_, ft_doc_word_iter_))) {
        } else if (OB_ISNULL(doc_word_info_)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(data_plane::init_ft_doc_word_iterator(
                               ft_doc_word_iter_,
                               doc_word_info_->doc_word_table_id_,
                               doc_word_info_->doc_word_tablet_id_,
                               &doc_word_info_->snapshot_,
                               doc_word_info_->doc_word_schema_version_))) {
        }
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      is_inited_ = true;
    }
  }
  if (OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObFTDMLIterator::change_domain_dml_mode(const ObDomainDMLMode &mode)
{
  int ret = OB_SUCCESS;
  const ObDomainDMLMode old_mode = mode_;
  if (OB_UNLIKELY(mode < ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT || mode >= ObDomainDMLMode::DOMAIN_DML_MODE_MAX)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(das_ctdef_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_fts_index())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObDomainDMLIterator::rewind())) {
  } else if (mode == mode_) {
    // nothing to do
  } else {
    switch (mode) {
      case ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT: {
        const common::ObString &parser_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_name();
        const common::ObString &parser_property_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_property();
        bool is_same = false;
        if (OB_FAIL(ft_parse_helper_.check_is_the_same(parser_str, parser_property_str, is_same))) {
        } else if (is_same) {
          // This is the same as the parser name of the previous index.
          // nothing to do, just skip.
        } else if (FALSE_IT(ft_parse_helper_.reset())) {
        } else if (OB_FAIL(ft_parse_helper_.init(&allocator_, parser_str, parser_property_str))) {
        }
        break;
      }
      case ObDomainDMLMode::DOMAIN_DML_MODE_FT_SCAN: {
        if (OB_ISNULL(doc_word_info_)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (FALSE_IT(data_plane::reset_ft_doc_word_iterator(ft_doc_word_iter_))) {
        } else if (OB_FAIL(data_plane::init_ft_doc_word_iterator(
                               ft_doc_word_iter_,
                               doc_word_info_->doc_word_table_id_,
                               doc_word_info_->doc_word_tablet_id_,
                               &doc_word_info_->snapshot_,
                               doc_word_info_->doc_word_schema_version_))) {
        }
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      mode_ = mode;
    }
  }
  return ret;
}

int ObFTDMLIterator::generate_domain_rows(const ObChunkDatumStore::StoredRow *store_row)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(store_row)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_fts_index())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    switch (mode_) {
      case ObDomainDMLMode::DOMAIN_DML_MODE_DEFAULT: {
        if (OB_FAIL(generate_ft_word_rows(store_row)) &&(ret != OB_ITER_END)) {
        }
        break;
      }
      case ObDomainDMLMode::DOMAIN_DML_MODE_FT_SCAN: {
        if (OB_FAIL(scan_ft_word_rows(store_row))) {
        }
        break;
      }
      default: {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

int ObFTDMLIterator::generate_ft_word_rows(const ObChunkDatumStore::StoredRow *store_row)
{
  int ret = OB_SUCCESS;
  common::ObObjMeta ft_meta;
  ObString ft;
  ObDatum doc_id_datum;
  const bool is_fts_index_aux = das_ctdef_->table_param_.get_data_table().is_fts_index_aux();

  if (!is_update_ && OB_FAIL(get_ft_and_doc_id(store_row, doc_id_datum, ft, ft_meta))) {
  } else if (is_update_ && OB_FAIL(get_ft_and_doc_id_for_update(store_row, doc_id_datum, ft, ft_meta))) {
  } else if (OB_FAIL(ObDASDomainUtils::generate_fulltext_word_rows(allocator_,
                                                                   &ft_parse_helper_,
                                                                   ft_meta,
                                                                   doc_id_datum,
                                                                   ft,
                                                                   is_fts_index_aux,
                                                                   rows_,
                                                                   &word_map_,
                                                                   true))) {
    if (OB_UNLIKELY(OB_ITER_END != ret)) {
    }
  }
  return ret;
}

int ObFTDMLIterator::scan_ft_word_rows(const ObChunkDatumStore::StoredRow *store_row)
{
  int ret = OB_SUCCESS;
  uint64_t doc_id_col_id = OB_INVALID_ID; // get to check
  ObDocIDType type;
  if (OB_FAIL(das_ctdef_->table_param_.get_data_table().get_typed_doc_id_col_id(doc_id_col_id, type))) {
  } else if (OB_UNLIKELY(OB_INVALID_ID == doc_id_col_id)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const bool is_fts_index_aux = das_ctdef_->table_param_.get_data_table().is_fts_index_aux();
    const int64_t doc_id_idx = !is_fts_index_aux ? 0 : 1;

    ObDatum doc_id_datum = store_row->cells()[row_projector_->at(doc_id_idx)];

    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(data_plane::scan_ft_doc_words(
                   ft_doc_word_iter_, doc_word_info_->doc_word_table_id_, doc_id_datum))) {
    } else {
      // gather ft word rows
      ObDomainIndexRow tmp_rows;
      do {
        blocksstable::ObDatumRow *row = nullptr;
        blocksstable::ObDatumRow *ft_word_row = nullptr;
        if (OB_FAIL(data_plane::next_ft_doc_word(ft_doc_word_iter_, row))) {
          if (OB_ITER_END != ret) {
          }
        } else if (OB_FAIL(build_ft_word_row(row, ft_word_row))) {
        } else if (OB_FAIL(tmp_rows.push_back(ft_word_row))) {
        } else {
        }
      } while (OB_SUCC(ret));

      // make it sequential
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        blocksstable::ObDatumRow *row_array = nullptr;
        if (0 == tmp_rows.count()) {
          // do nothing
        } else if (OB_ISNULL(row_array = OB_NEW_ARRAY(blocksstable::ObDatumRow, &allocator_, tmp_rows.count()))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int i = 0; OB_SUCC(ret) && i < tmp_rows.count(); ++i) {
            if (OB_ISNULL(tmp_rows[i])) {
              ret = OB_ERR_UNEXPECTED;
            } else if (OB_FAIL(row_array[i].shallow_copy(*(tmp_rows[i])))) {
            } else if (OB_FAIL(rows_.push_back(&row_array[i]))) {
            }
          }

          if (OB_SUCC(ret)) {
            LOG_TRACE("succeed to scan ft word rows", K(rows_.count()));
          }
        }
      }
    }
  }
  if (OB_SUCC(ret) && (config::_enable_defensive_check() == 2)) {
    common::ObArenaAllocator allocator(lib::ObMemAttr("FTIterDEF"));
    const common::ObString &parser_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_name();
    const common::ObString &parser_property_str = das_ctdef_->table_param_.get_data_table().get_fts_parser_property();
    storage::ObFTParseHelper ft_parse_helper;
    ObDatum check_id_datum;
    ObString doc_id;
    ObString ft;
    common::ObObjMeta ft_meta;
    ObDomainIndexRow rows;
    const bool is_fts_index_aux = das_ctdef_->table_param_.get_data_table().is_fts_index_aux();
    if (OB_FAIL(ft_parse_helper.init(&allocator, parser_str, parser_property_str))) {
    } else if (!is_update_ && OB_FAIL(get_ft_and_doc_id(store_row, check_id_datum, ft, ft_meta))) {
    } else if (is_update_ && OB_FAIL(get_ft_and_doc_id_for_update(store_row, check_id_datum, ft, ft_meta))) {
    } else if (OB_FAIL(ObDASDomainUtils::generate_fulltext_word_rows(allocator,
                                                                     &ft_parse_helper,
                                                                     ft_meta,
                                                                     check_id_datum,
                                                                     ft,
                                                                     is_fts_index_aux,
                                                                     rows,
                                                                     nullptr))) {
      if (OB_UNLIKELY(OB_ITER_END != ret)) {
      }
    }
    if (OB_ITER_END == ret || OB_SUCC(ret)) {
      ret = OB_SUCCESS;
      if (OB_UNLIKELY(rows_.count() != rows.count())) {
        bool need_check_tx_active = true;
        if (OB_NOT_NULL(doc_word_info_) && doc_word_info_->snapshot_.tx_id().is_valid()) {
          const transaction::ObTransID &tx_id = doc_word_info_->snapshot_.tx_id();
          //data_plane::ObITransactionService *txs = data_plane::query_transaction_service();
          //bool is_tx_active = true;
          //if (OB_NOT_NULL(txs)) {
          //  if (OB_FAIL(txs->is_tx_active(tx_id, is_tx_active))) {
          //    LOG_WARN("fail to check if tx is active", K(ret), K(tx_id));
          //  } else if (!is_tx_active) {
          //    need_check_tx_active = false;
          //    LOG_WARN("tx is aborted, skip this check.", K(tx_id));
          //  }
          //}
        }

        if (OB_SUCC(ret) && need_check_tx_active) {
          ret = OB_ERR_DEFENSIVE_CHECK;
          common::ObDocId docid;
          docid.from_string(doc_id);
          LOG_ERROR("row count isn't equal between scan ft words and generate ft words", K(ret), K(rows_), K(rows),
              K(is_fts_index_aux), K(doc_id), K(docid), K(ft_meta), K(ft), K(ft_parse_helper), KPC(store_row),
              KPC(main_ctdef_));
        }
      }
    }
  }
  return ret;
}

int ObFTDMLIterator::build_ft_word_row(
    blocksstable::ObDatumRow *src_row,
    blocksstable::ObDatumRow *&dest_row)
{
  int ret = OB_SUCCESS;
  const int64_t DOC_ID_IDX = das_ctdef_->table_param_.get_data_table().is_fts_index_aux() ? 1 : 0;
  const int64_t WORD_SEGMENT_IDX = das_ctdef_->table_param_.get_data_table().is_fts_index_aux() ? 0 : 1;
  static int64_t WORD_COUNT_IDX = 2;
  static int64_t DOC_LENGTH_IDX = 3;
  void *buf = nullptr;
  blocksstable::ObDatumRow *tmp_row = nullptr;
  if (OB_ISNULL(src_row)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(4 != src_row->count_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = allocator_.alloc(sizeof(blocksstable::ObDatumRow)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (FALSE_IT(tmp_row = new (buf) blocksstable::ObDatumRow())) {
  } else if (OB_FAIL(tmp_row->init(src_row->count_))) {
  } else if (OB_FAIL(tmp_row->copy_attributes_except_datums(*src_row))) {
  } else if (OB_FAIL(tmp_row->storage_datums_[DOC_ID_IDX].deep_copy(src_row->storage_datums_[0], allocator_))) {
  } else if (OB_FAIL(tmp_row->storage_datums_[WORD_SEGMENT_IDX].deep_copy(src_row->storage_datums_[1], allocator_))) {
  } else if (OB_FAIL(tmp_row->storage_datums_[WORD_COUNT_IDX].deep_copy(src_row->storage_datums_[2], allocator_))) {
  } else if (OB_FAIL(tmp_row->storage_datums_[DOC_LENGTH_IDX].deep_copy(src_row->storage_datums_[3], allocator_))) {
  } else {
    dest_row = tmp_row;
  }
  return ret;
}

int ObFTDMLIterator::get_ft_and_doc_id(const ObChunkDatumStore::StoredRow *store_row,
                                       ObDatum &doc_id_datum,
                                       ObString &ft,
                                       common::ObObjMeta &ft_meta)
{
  int ret = OB_SUCCESS;
  const uint64_t fts_col_id = das_ctdef_->table_param_.get_data_table().get_fulltext_col_id();
  if (OB_UNLIKELY(OB_INVALID_ID == fts_col_id)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const bool is_fts_index_aux = das_ctdef_->table_param_.get_data_table().is_fts_index_aux();
    const int64_t doc_id_idx = !is_fts_index_aux ? 0 : 1;
    const int64_t ft_idx = !is_fts_index_aux ? 1 : 0;

    ft = store_row->cells()[row_projector_->at(ft_idx)].get_string();
    ft_meta = das_ctdef_->column_types_.at(ft_idx);
    doc_id_datum = store_row->cells()[row_projector_->at(doc_id_idx)];

    if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                                                          *lob_read_options_,
                                                          &allocator_,
                                                          ft_meta.get_type(),
                                                          ft_meta.get_collation_type(),
                                                          ft_meta.has_lob_header(),
                                                          ft))) {
    } else {
    }
  }
  return ret;
}

int ObFTDMLIterator::get_ft_and_doc_id_for_update(const ObChunkDatumStore::StoredRow *store_row,
                                                  ObDatum &doc_id_datum,
                                                  ObString &ft,
                                                  common::ObObjMeta &ft_meta)
{
  int ret = OB_SUCCESS;
  const uint64_t rowkey_col_cnt = das_ctdef_->table_param_.get_data_table().get_rowkey_column_num();
  const uint64_t old_proj_cnt = das_ctdef_->old_row_projector_.count();
  const uint64_t new_proj_cnt = das_ctdef_->new_row_projector_.count();
  if (OB_UNLIKELY(rowkey_col_cnt + 2 != old_proj_cnt || rowkey_col_cnt + 2 != new_proj_cnt)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    const bool is_fts_index_aux = das_ctdef_->table_param_.get_data_table().is_fts_index_aux();
    const int64_t doc_id_idx = !is_fts_index_aux ? 0 : 1;
    const int64_t ft_idx = !is_fts_index_aux ? 1 : 0;

    doc_id_datum = store_row->cells()[row_projector_->at(doc_id_idx)];

    ft = store_row->cells()[row_projector_->at(ft_idx)].get_string();
    ft_meta = das_ctdef_->column_types_.at(ft_idx);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                                                                 *lob_read_options_,
                                                                 &allocator_,
                                                                 ft_meta.get_type(),
                                                                 ft_meta.get_collation_type(),
                                                                 true /* has lob header */,
                                                                 ft))) {
    } else {
    }
  }
  return ret;
}

int ObMultivalueDMLIterator::generate_domain_rows(const ObChunkDatumStore::StoredRow *store_row)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(store_row)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(!das_ctdef_->table_param_.get_data_table().is_multivalue_index_aux())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    int64_t multivalue_idx = OB_INVALID_ID;
    int64_t multivalue_arr_idx = OB_INVALID_ID;
    ObString multivalue_data;
    if (!is_update_ && OB_FAIL(get_multivlaue_json_data(
      store_row, multivalue_idx, multivalue_arr_idx, multivalue_data))) {
    } else if (is_update_ && OB_FAIL(get_multivlaue_json_data_for_update(
      store_row, multivalue_idx, multivalue_arr_idx, multivalue_data))) {
    } else if (OB_FAIL(ObDASDomainUtils::generate_multivalue_index_rows(
      allocator_, *das_ctdef_, multivalue_idx, multivalue_arr_idx,
      multivalue_data, *row_projector_, *store_row, rows_))) {
      if (ret != OB_ITER_END) {
      }
    }
  }
  return ret;
}

int ObMultivalueDMLIterator::get_multivlaue_json_data(
    const ObChunkDatumStore::StoredRow *store_row,
    int64_t& multivalue_idx,
    int64_t& multivalue_arr_idx,
    ObString &multivalue_data)
{
  int ret = OB_SUCCESS;
  multivalue_idx = OB_INVALID_ID;

  const uint64_t multivalue_col_id = das_ctdef_->table_param_.get_data_table().get_multivalue_col_id();
  bool found = false;

  if (OB_INVALID_ID == multivalue_col_id) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < das_ctdef_->column_ids_.count() && !found; ++i) {
      const int64_t column_id = das_ctdef_->column_ids_.at(i);
      const int64_t projector_idx = row_projector_->at(i);
      if (OB_UNLIKELY(projector_idx >= store_row->cnt_)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (multivalue_col_id == column_id) {
        found = true;
        multivalue_idx = projector_idx;
        multivalue_arr_idx = multivalue_idx + 1;
        multivalue_data = store_row->cells()[multivalue_arr_idx].get_string();

        if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                                                            *lob_read_options_,
                                                            &allocator_,
                                                            ObJsonType,
                                                            CS_TYPE_UTF8MB4_BIN,
                                                            true, multivalue_data))) {
        }
      }
    }

    if (OB_SUCC(ret) && !found) {
      ret = OB_INVALID_ARGUMENT;
    }
  }

  return ret;
}

int ObMultivalueDMLIterator::get_multivlaue_json_data_for_update(
    const ObChunkDatumStore::StoredRow *store_row,
    int64_t& multivalue_idx,
    int64_t& multivalue_arr_idx,
    ObString &multivalue_data)
{
  int ret = OB_SUCCESS;
  bool found = false;

  multivalue_idx = OB_INVALID_ID;
  multivalue_arr_idx = OB_INVALID_ID;

  const uint64_t multivalue_col_id = das_ctdef_->table_param_.get_data_table().get_multivalue_col_id();

  for (int64_t i = 0; OB_SUCC(ret) && i < das_ctdef_->column_ids_.count() && !found; ++i) {
    const int64_t projector_idx = row_projector_->at(i);
    if (multivalue_col_id == das_ctdef_->column_ids_.at(i)) {
      if (projector_idx >= store_row->cnt_) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        found = true;
        multivalue_idx = projector_idx;

        if (projector_idx >= store_row->cnt_) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          multivalue_arr_idx = projector_idx + 1;
          multivalue_data = store_row->cells()[multivalue_arr_idx].get_string();

          if (OB_FAIL(ObTextStringHelper::read_real_string_data(
                                                              *lob_read_options_,
                                                              &allocator_,
                                                              ObJsonType,
                                                              CS_TYPE_UTF8MB4_BIN,
                                                              true, multivalue_data))) {
          }
        }
      }
    }
  }

  if (OB_SUCC(ret) && !found) {
    ret = OB_INVALID_ARGUMENT;
  }

  return ret;
}

} // end namespace storage
} // end namespace oceanbase
