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

#pragma once

#include "lib/allocator/ob_allocator.h"
#include "lib/list/ob_list.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_fast_dict.h"
#include "storage/fts/ik/ob_ik_hit_array.h"
#include "storage/fts/ik/ob_ik_processor.h"

namespace oceanbase
{
namespace storage
{
class ObFTDictHub;

/**
 * @class IKFastHitArray
 * @brief Pending matches of the compact trie path, in insertion order.
 *
 * Same contract as IKHitArray (fixed array, insertion order preserved, overflow
 * migrated to an ObList so that no match is ever dropped), but it stores the
 * compact trie node id instead of a double array trie hit.
 *
 * An IK word is at most 16 characters long, so at most 15 matches are pending at
 * any time and the list is never reached; it is kept so that the code has no
 * silent capacity limit.
 */
class IKFastHitArray final
{
public:
  static constexpr int64_t CAPACITY = 32;

  struct Hit
  {
    int64_t start_;   // byte offset of the first character of the word
    uint32_t node_;   // trie node reached so far
    uint32_t char_cnt_;
  };

public:
  explicit IKFastHitArray(ObIAllocator &alloc) : alloc_(alloc), list_(alloc), size_(0), overflow_(false)
  {
  }
  ~IKFastHitArray() { clear(); }

  int64_t size() const { return overflow_ ? list_.size() : size_; }
  bool empty() const { return 0 == size(); }

  Hit &at(const int64_t idx)
  {
    if (OB_UNLIKELY(overflow_)) {
      ObList<Hit, ObIAllocator>::iterator iter = list_.begin();
      for (int64_t i = 0; i < idx; ++i) {
        ++iter;
      }
      return *iter;
    }
    return raw_[idx];
  }

  int push_back(const Hit &hit)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(overflow_)) {
      ret = list_.push_back(hit);
    } else if (size_ < CAPACITY) {
      raw_[size_++] = hit;
    } else if (OB_FAIL(migrate_to_list())) {
    } else {
      ret = list_.push_back(hit);
    }
    return ret;
  }

  // Erases the hit at idx keeping the order of the remaining hits.
  int erase_at(const int64_t idx)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(idx < 0 || idx >= size())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_UNLIKELY(overflow_)) {
      ObList<Hit, ObIAllocator>::iterator iter = list_.begin();
      for (int64_t i = 0; i < idx; ++i) {
        ++iter;
      }
      ret = list_.erase(iter);
    } else {
      for (int64_t i = idx + 1; i < size_; ++i) {
        raw_[i - 1] = raw_[i];
      }
      --size_;
    }
    return ret;
  }

  void clear()
  {
    if (OB_UNLIKELY(overflow_)) {
      list_.clear();
      overflow_ = false;
    }
    size_ = 0;
  }

private:
  int migrate_to_list()
  {
    int ret = OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < size_; ++i) {
      if (OB_FAIL(list_.push_back(raw_[i]))) {
      }
    }
    if (OB_FAIL(ret)) {
      list_.clear();
    } else {
      size_ = 0;
      overflow_ = true;
    }
    return ret;
  }

private:
  ObIAllocator &alloc_;
  ObList<Hit, ObIAllocator> list_;
  int64_t size_;
  bool overflow_;
  Hit raw_[CAPACITY];

private:
  DISALLOW_COPY_AND_ASSIGN(IKFastHitArray);
};

class ObIKCJKProcessor : public ObIIKProcessor
{
public:
  ObIKCJKProcessor(const ObIFTDict &dict_main, ObIAllocator &alloc)
      : hits_(alloc), fast_hits_(alloc), dict_main_(dict_main), cjk_start_(-1), cjk_end_(-1),
        fast_dict_(nullptr)
  {
  }
  ~ObIKCJKProcessor() override = default;

  // Non null selects the compact trie path (see IKFastDict); nullptr keeps the
  // double array trie implementation.
  void set_fast_dict(const IKFastDict *fast_dict) { fast_dict_ = fast_dict; }

public:
  int do_process(TokenizeContext &ctx,
                 const char *ch,
                 const uint8_t char_len,
                 const ObFTCharUtil::CharType type) override;

  // True while a dictionary prefix match is pending, which is the only state a
  // USELESS character can affect.
  bool has_pending_hits() const
  {
    return OB_NOT_NULL(fast_dict_) ? !fast_hits_.empty() : !hits_.empty();
  }

private:
  int do_process_dat(TokenizeContext &ctx,
                     const char *ch,
                     const uint8_t char_len,
                     const ObFTCharUtil::CharType type);
  int do_process_fast(TokenizeContext &ctx,
                      const char *ch,
                      const uint8_t char_len,
                      const ObFTCharUtil::CharType type);

private:
  IKHitArray hits_;
  IKFastHitArray fast_hits_;
  const ObIFTDict &dict_main_;
  int64_t cjk_start_;
  int64_t cjk_end_;
  const IKFastDict *fast_dict_;

private:
  DISALLOW_COPY_AND_ASSIGN(ObIKCJKProcessor);
};

} //  namespace storage
} //  namespace oceanbase
