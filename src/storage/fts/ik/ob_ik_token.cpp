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

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_token.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"

namespace oceanbase
{
namespace storage
{
int ObFTSortList::add_token(const ObIKToken &token)
{
  int ret = OB_SUCCESS;

  if (tokens_.empty()) {
    if (OB_FAIL(tokens_.push_back(token))) {
    }
  } else if (tokens_.get_last() == token) {
    // pass
  } else if (token > tokens_.get_last()) {
    if (OB_FAIL(tokens_.push_back(token))) {
    }
  } else if (token < tokens_.get_first()) {
    if (OB_FAIL(tokens_.push_front(token))) {
    }
  } else {
    for (ObFTSortList::CellIter iter = tokens_.last(); OB_SUCC(ret) && iter != tokens_.end();
         --iter) {
      if (token < *iter) {
        continue;
      }
      if (*iter == token) {
        // no need to add again
      } else if (OB_FAIL(tokens_.insert(++iter, token))) {
      } else {
        // insert ok
      }
      break;
    }
  }

  return ret;
}

int ObIKTokenChain::add_token_if_conflict(const ObIKToken &token, bool &added)
{
  int ret = OB_SUCCESS;
  added = true;
  if (list_.is_empty()) {
    const int64_t old_size = list_.tokens().size();
    list_.add_token(token);
    min_offset_ = token.offset_;
    max_offset_ = token.offset_ + token.length_;
    payload_ = token.length_;
    if (list_.tokens().size() != old_size) {
      update_weight_cache_after_append(token);
    }
  } else if (check_conflict(token)) {
    const int64_t old_size = list_.tokens().size();
    const bool append = token > list_.tokens().get_last();
    if (append && !weight_cache_valid_) {
      rebuild_weight_cache();
    }
    list_.add_token(token);
    if (token.offset_ + token.length_ > max_offset_) {
      max_offset_ = token.offset_ + token.length_;
    }
    payload_ = max_offset_ - min_offset_;
    if (list_.tokens().size() != old_size) {
      update_weight_cache_after_insert(token, append);
    }
  } else {
    added = false;
  }
  return ret;
}

int ObIKTokenChain::add_token_if_no_conflict(const ObIKToken &token, bool &added)
{
  int ret = OB_SUCCESS;
  added = true;
  if (list_.is_empty()) {
    const int64_t old_size = list_.tokens().size();
    list_.add_token(token);
    min_offset_ = token.offset_;
    max_offset_ = token.offset_ + token.length_;
    payload_ = token.length_;
    if (list_.tokens().size() != old_size) {
      update_weight_cache_after_append(token);
    }
  } else if (check_conflict(token)) {
    added = false;
  } else {
    const int64_t old_size = list_.tokens().size();
    const bool append = token > list_.tokens().get_last();
    if (append && !weight_cache_valid_) {
      rebuild_weight_cache();
    }
    list_.add_token(token);
    min_offset_ = list_.min();
    max_offset_ = list_.max();
    payload_ += token.length_;
    if (list_.tokens().size() != old_size) {
      update_weight_cache_after_insert(token, append);
    }
    added = true;
  }
  return ret;
}

bool ObIKTokenChain::check_conflict(const ObIKToken &token)
{
  if (list_.is_empty()) {
    return false;
  }
  return (token.offset_ >= min_offset_ && token.offset_ < max_offset_)
         || (min_offset_ >= token.offset_ && min_offset_ < token.offset_ + token.length_);
}

bool ObIKTokenChain::better_than(const ObIKTokenChain &other) const
{
  if (this == &other) {
    return false;
  }
  // more valid text length
  if (payload_ != other.payload_) {
    return payload_ > other.payload_;
  }

  // less words
  if (token_count_ != other.token_count_) {
    return token_count_ < other.token_count_;
  }

  // more distance
  if (offset_len() != other.offset_len()) {
    return offset_len() > other.offset_len();
  }

  // inverse cut is better
  if (max_offset_ != other.max_offset_) {
    return max_offset_ > other.max_offset_;
  }

  // more average distance is better
  const int64_t this_x_weight = x_weight();
  const int64_t other_x_weight = other.x_weight();
  if (this_x_weight != other_x_weight) {
    return this_x_weight > other_x_weight;
  }

  // position
  return p_weight() > other.p_weight();
}

int ObIKTokenChain::copy(ObIKTokenChain *other)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(other)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    min_offset_ = other->min_offset_;
    max_offset_ = other->max_offset_;
    payload_ = other->payload_;
    ObFTSortList::ConstCellIter iter = other->list().tokens().begin();

    for (; OB_SUCC(ret) && iter != other->list().tokens().end(); ++iter) {
      bool added = false;
      if (OB_FAIL(add_token_if_no_conflict(*iter, added))) {
      }
    }
  }
  return ret;
}

int64_t ObIKTokenChain::p_weight() const
{
  if (!weight_cache_valid_) {
    rebuild_weight_cache();
  }
  return p_weight_cache_;
}
int64_t ObIKTokenChain::x_weight() const
{
  if (!weight_cache_valid_) {
    rebuild_weight_cache();
  }
  return x_weight_cache_;
}

int ObIKTokenChain::pop_back(ObIKToken &token)
{
  int ret = OB_SUCCESS;
  if (list_.tokens().empty()) {
    ret = OB_ENTRY_NOT_EXIST;
  } else if (FALSE_IT(token = list_.tokens().get_last())) {
  } else if (OB_FAIL(list_.tokens().pop_back())) {
  } else if (list_.tokens().empty()) {
    min_offset_ = -1;
    max_offset_ = -1;
    payload_ = 0;
    reset_weight_cache();
  } else {
    --token_count_;
    weight_cache_valid_ = false;
    payload_ -= token.length_;
    ObIKToken new_last = list_.tokens().get_last();
    // only for not conflict case
    max_offset_ = new_last.offset_ + new_last.length_;
  }
  return ret;
}

void ObIKTokenChain::reset_weight_cache()
{
  token_count_ = 0;
  position_count_ = 0;
  x_weight_cache_ = 1;
  p_weight_cache_ = 0;
  weight_cache_valid_ = true;
}

void ObIKTokenChain::rebuild_weight_cache() const
{
  token_count_ = 0;
  position_count_ = 0;
  x_weight_cache_ = 1;
  p_weight_cache_ = 0;
  int p = 0;
  for (const ObIKToken &token : list_.tokens()) {
    ++token_count_;
    x_weight_cache_ *= token.char_cnt_;
    ++p;
    p_weight_cache_ += p * token.char_cnt_;
  }
  position_count_ = p;
  weight_cache_valid_ = true;
}

void ObIKTokenChain::update_weight_cache_after_insert(const ObIKToken &token, bool append)
{
  if (append) {
    update_weight_cache_after_append(token);
  } else {
    rebuild_weight_cache();
  }
}

void ObIKTokenChain::update_weight_cache_after_append(const ObIKToken &token)
{
  ++token_count_;
  x_weight_cache_ *= token.char_cnt_;
  ++position_count_;
  p_weight_cache_ += position_count_ * token.char_cnt_;
}

int64_t ObFTSortList::max()
{
  if (tokens_.empty()) {
    return 0;
  }
  return tokens_.get_last().offset_ + tokens_.get_last().length_;
}
int64_t ObFTSortList::min()
{
  if (tokens_.empty()) {
    return 0;
  }
  return tokens_.get_first().offset_;
}
} //  namespace storage
} //  namespace oceanbase
