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

#include <new>

#include "lib/allocator/ob_allocator.h"
#include "lib/list/ob_list.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"

namespace oceanbase
{
namespace storage
{
/**
 * @class IKHitArray
 * @brief Pending DATrie hits of one IK processor, in insertion order.
 *
 * A Chinese word spans a handful of characters, so only a few hits are pending
 * at any time.  The processors used to keep them in an ObList and therefore had
 * to allocate (and free) one list node per character of the input.  The hits now
 * live in a fixed size array that is walked in insertion order, which is the
 * same order the ObList produced, so the token stream is unchanged.
 *
 * If a document ever has more than CAPACITY pending hits, the contents are moved
 * to an ObList and the remaining pushes keep using it: no hit is dropped, and
 * the order of the sequence (and hence of the emitted tokens) is preserved.
 * Erasing compacts the array in place instead of freeing a node.
 */
class IKHitArray final
{
public:
  static constexpr int64_t CAPACITY = 32;

public:
  explicit IKHitArray(ObIAllocator &alloc)
      : alloc_(alloc), list_(alloc), size_(0), overflow_(false)
  {
  }
  ~IKHitArray() { clear(); }

  int64_t size() const { return overflow_ ? list_.size() : size_; }
  bool empty() const { return 0 == size(); }

  ObDATrieHit &at(const int64_t idx)
  {
    ObDATrieHit *hit = nullptr;
    if (OB_UNLIKELY(overflow_)) {
      ObList<ObDATrieHit, ObIAllocator>::iterator iter = list_.begin();
      for (int64_t i = 0; i < idx; ++i) {
        ++iter;
      }
      hit = &(*iter);
    } else {
      hit = slot(idx);
    }
    return *hit;
  }

  int push_back(const ObDATrieHit &hit)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(overflow_)) {
      ret = list_.push_back(hit);
    } else if (size_ < CAPACITY) {
      // Copy construction copies dict_ as well, which match_with_hit needs.
      new (slot(size_)) ObDATrieHit(hit);
      ++size_;
    } else if (OB_FAIL(migrate_to_list())) {
    } else {
      ret = list_.push_back(hit);
    }
    return ret;
  }

  // Erases the hit at idx, keeping the order of the remaining hits.
  int erase_at(const int64_t idx)
  {
    int ret = OB_SUCCESS;
    if (OB_UNLIKELY(idx < 0 || idx >= size())) {
      ret = OB_INVALID_ARGUMENT;
    } else if (overflow_) {
      ObList<ObDATrieHit, ObIAllocator>::iterator iter = list_.begin();
      for (int64_t i = 0; i < idx; ++i) {
        ++iter;
      }
      ret = list_.erase(iter);
    } else {
      // operator= does not copy dict_, move the whole hit explicitly.
      for (int64_t i = idx + 1; i < size_; ++i) {
        slot(i - 1)->assign_all(*slot(i));
      }
      --size_;
      slot(size_)->~ObDATrieHit();
    }
    return ret;
  }

  void clear()
  {
    if (overflow_) {
      list_.clear();
      overflow_ = false;
    } else {
      for (int64_t i = 0; i < size_; ++i) {
        slot(i)->~ObDATrieHit();
      }
      size_ = 0;
    }
  }

private:
  ObDATrieHit *slot(const int64_t idx)
  {
    return reinterpret_cast<ObDATrieHit *>(raw_) + idx;
  }

  // Moves every pending hit to the list, keeping insertion order.  On failure the
  // list is emptied again so that the array stays the single source of truth and
  // the caller can simply drop the whole document state.
  int migrate_to_list()
  {
    int ret = OB_SUCCESS;
    for (int64_t i = 0; OB_SUCC(ret) && i < size_; ++i) {
      if (OB_FAIL(list_.push_back(*slot(i)))) {
      }
    }
    if (OB_FAIL(ret)) {
      list_.clear();
    } else {
      for (int64_t i = 0; i < size_; ++i) {
        slot(i)->~ObDATrieHit();
      }
      size_ = 0;
      overflow_ = true;
    }
    return ret;
  }

private:
  ObIAllocator &alloc_;
  // The list is only used once more than CAPACITY hits are pending.  It is kept
  // as a member so that the fallback needs no allocation of its own.
  ObList<ObDATrieHit, ObIAllocator> list_;
  int64_t size_;
  bool overflow_;
  // Storage for the hits that is only initialized on push_back, so an idle
  // processor costs nothing beyond its size.
  alignas(ObDATrieHit) char raw_[sizeof(ObDATrieHit) * CAPACITY];

private:
  DISALLOW_COPY_AND_ASSIGN(IKHitArray);
};

} // namespace storage
} // namespace oceanbase
