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

#include "storage/fts/ik/ob_ik_arbitrator.h"

#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIKArbitrator::process(TokenizeContext &ctx)
{
  int ret = OB_SUCCESS;

  ObList<ObIKToken, ObIAllocator> &tokens = ctx.token_list().tokens();
  ObIKTokenChain *chain_need_arbitrate = nullptr;
  bool use_smart = ctx.is_smart();
  if (OB_FAIL(prepare(ctx))) {
  } else if (OB_ISNULL(chain_need_arbitrate = OB_NEWx(ObIKTokenChain, &alloc_, alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc memory failed");
  } else {
    while (OB_SUCC(ret) && !tokens.empty()) {
      ObIKToken token = tokens.get_first();
      bool is_add = false;
      if (OB_FAIL(tokens.pop_front())) {
      } else if (OB_FAIL(chain_need_arbitrate->add_token_if_conflict(token, is_add))) {
      } else if (!is_add) {
        ObFTSortList::ConstCellIter iter = chain_need_arbitrate->list().tokens().begin();
        ObIKTokenChain *judge_result = nullptr;
        if (chain_need_arbitrate->list().tokens().size() == 1 || !use_smart) {
          if (OB_FAIL(add_chain(chain_need_arbitrate))) {
          } else {
            // add ok
          }
        } else if (OB_FAIL(optimize(ctx,
                                    chain_need_arbitrate,
                                    iter,
                                    chain_need_arbitrate->offset_len(),
                                    judge_result))) {
        } else if (OB_FAIL(add_chain(judge_result))) {
        } else {
          // add best chain and delete origin chain
          OB_DELETEx(ObIKTokenChain, &alloc_, chain_need_arbitrate);
        }

        // start a new chain
        if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(chain_need_arbitrate = OB_NEWx(ObIKTokenChain, &alloc_, alloc_))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("alloc memory failed");
        } else if (OB_FAIL(chain_need_arbitrate->add_token_if_conflict(token, is_add))) {
        } else {
          // Add first, it should be added.
        }
      } else {
        // still use same cross chain
      }
    }

    // handle last chain
    ObIKTokenChain *judge_result = nullptr;
    if (OB_FAIL(ret)) {
    } else if (chain_need_arbitrate->list().tokens().size() == 1 || !use_smart) {
      if (OB_FAIL(add_chain(chain_need_arbitrate))) {
      }
    } else if (OB_FAIL(optimize(ctx,
                                chain_need_arbitrate,
                                chain_need_arbitrate->list().tokens().begin(),
                                chain_need_arbitrate->offset_len(),
                                judge_result))) {
    } else if (OB_FAIL(add_chain(judge_result))) {
      OB_DELETEx(ObIKTokenChain, &alloc_, judge_result);
    } else {
    }
  }

  // if failed, free chain_need_arbitrate
  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIKTokenChain, &alloc_, chain_need_arbitrate);
  }

  return ret;
}

namespace
{
// Length and class of the character that starts at `offset`.  UTF8MB4 is the
// only charset the IK parser accepts, and for it one decode yields both values;
// anything the fast path cannot decode (malformed bytes) keeps the original two
// step handling, including the error it reports.
int classify_char_at(const TokenizeContext &ctx,
                     const int64_t offset,
                     int64_t &char_len,
                     ObFTCharUtil::CharType &type)
{
  int ret = OB_SUCCESS;
  if (CHARSET_UTF8MB4 == ctx.charset_type()
      && ObFTCharUtil::classify_first_valid_utf8mb4_char(ctx.fulltext() + offset,
                                                         ctx.fulltext_len() - offset,
                                                         char_len,
                                                         type)) {
    // done
  } else if (OB_FAIL(ctx.first_valid_char_at(offset, char_len))) {
  } else if (OB_FAIL(ObFTCharUtil::classify_first_char(ctx.charset_type(),
                                                       ctx.fulltext() + offset,
                                                       char_len,
                                                       type))) {
  }
  return ret;
}
} // namespace

int ObIKArbitrator::output_result(TokenizeContext &ctx)
{
  int ret = OB_SUCCESS;

  int64_t char_len = 0;
  ObIKTokenChain *chain = nullptr;
  for (int64_t current = 0; OB_SUCC(ret) && current < ctx.fulltext_len();) {
    ObFTCharUtil::CharType type;
    // maybe not so good to keep single, check it later
    if (OB_FAIL(classify_char_at(ctx, current, char_len, type))) {
    } else if (ObFTCharUtil::CharType::USELESS == type) {
      current += char_len; // skip useless char
    } else if (OB_FAIL(lookup_chain(current, chain)) && OB_HASH_NOT_EXIST != ret) {
    } else if (OB_HASH_NOT_EXIST == ret) {
      ret = OB_SUCCESS;
      if (!keep_single()) {
        // skip single word
      } else {
        // output single word
        ObIKToken token;
        token.offset_ = current;
        token.length_ = char_len;
        token.ptr_ = ctx.fulltext();
        token.char_cnt_ = 1;
        bool is_ignore = false;
        if (ObFTCharUtil::CharType::CHINESE == type) {
          token.type_ = ObIKTokenType::IK_CHINESE_TOKEN;
          if (OB_FAIL(ctx.result_list().push_back(token))) {
          }
        } else if (ObFTCharUtil::CharType::OTHER_CJK == type) {
          if (OB_FAIL(ObFTCharUtil::is_ignore_single_cjk(ctx.charset_type(),
                                                         ctx.fulltext() + current,
                                                         char_len,
                                                         is_ignore))) {
          } else if (!is_ignore && !FALSE_IT(token.type_ = ObIKTokenType::IK_OTHER_CJK_TOKEN)
                     && OB_FAIL(ctx.result_list().push_back(token))) {
          } else {
            // ignore
          }
        }
      }
      current += char_len;
    } else {
      for (ObList<ObIKToken, ObIAllocator>::const_iterator iter = chain->list().tokens().begin();
           OB_SUCC(ret) && iter != chain->list().tokens().end();
           iter++) {
        const ObIKToken &token = *iter;
        if (!keep_single()) {
          current = token.offset_;
        } else {
          // output single word between two token
          while (OB_SUCC(ret) && current < token.offset_) {
            if (OB_FAIL(ctx.first_valid_char_at(current, char_len))) {
              break;
            } else {
              ObIKToken token;
              token.offset_ = current;
              token.length_ = char_len;
              token.ptr_ = ctx.fulltext();
              token.char_cnt_ = 1;
              bool is_ignore = false;
              if (ObFTCharUtil::CharType::CHINESE == type) {
                token.type_ = ObIKTokenType::IK_CHINESE_TOKEN;
                ctx.result_list().push_back(token);
              } else if (ObFTCharUtil::CharType::OTHER_CJK == type) {
                if (OB_FAIL(ObFTCharUtil::is_ignore_single_cjk(ctx.charset_type(),
                                                               ctx.fulltext() + current,
                                                               char_len,
                                                               is_ignore))) {
                } else if (!is_ignore) {
                  token.type_ = ObIKTokenType::IK_OTHER_CJK_TOKEN;
                  ctx.result_list().push_back(token);
                } else {
                }
              }
              current += char_len;
            }
          }
        }

        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ctx.result_list().push_back(token))) {
        } else
          // output the token
          current = token.offset_ + token.length_;
      }
    }
    if (OB_FAIL(ret)) {
      break;
    } else {
      // pass
    }
  }

  return ret;
}

int ObIKArbitrator::optimize(TokenizeContext &ctx,
                             ObIKTokenChain *chain,
                             ObFTSortList::ConstCellIter iter,
                             int64_t fulltext_len,
                             ObIKTokenChain *&best)
{
  int ret = OB_SUCCESS;

  ObIKTokenChain *option = nullptr;
  ObList<ObFTSortList::ConstCellIter, ObIAllocator> conflict_stack(alloc_);

  if (OB_ISNULL(option = OB_NEWx(ObIKTokenChain, &alloc_, alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("alloc memory failed");
  } else if (OB_FAIL(try_add_next_words(chain, iter, option, true, conflict_stack))) {
  } else if (OB_ISNULL(best = OB_NEWx(ObIKTokenChain, &alloc_, alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("Alloc memory failed");
  } else if (OB_FAIL(best->copy(option))) {
  } else {
    while (OB_SUCC(ret) && !conflict_stack.empty()) {
      ObFTSortList::ConstCellIter iter = conflict_stack.get_last();
      conflict_stack.pop_back();
      if (OB_FAIL(remove_conflict(*iter, option))) {
      } else if (OB_FAIL(try_add_next_words(chain, iter, option, false, conflict_stack))) {
      } else if (option->better_than(*best)) {
        OB_DELETEx(ObIKTokenChain, &alloc_, best);
        if (OB_ISNULL(best = OB_NEWx(ObIKTokenChain, &alloc_, alloc_))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          LOG_WARN("Alloc memory failed");
        } else if (OB_FAIL(best->copy(option))) {
        } else {
          // best option is copied
        }
      } else {
        // ignore worse chain
      }
    }
  }

  // any way, free option
  OB_DELETEx(ObIKTokenChain, &alloc_, option);

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIKTokenChain, &alloc_, best);
  }
  return ret;
}

int ObIKArbitrator::try_add_next_words(ObIKTokenChain *chain,
                                       ObFTSortList::ConstCellIter iter,
                                       ObIKTokenChain *option,
                                       bool need_conflict,
                                       ObList<ObFTSortList::ConstCellIter, ObIAllocator> &conflict_stack)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(chain) || OB_ISNULL(option)) {
    ret = OB_INVALID_ARGUMENT;
  }

  while (OB_SUCC(ret) && iter != chain->list().tokens().end()) {
    bool is_add = false;
    if (OB_FAIL(option->add_token_if_no_conflict(*iter, is_add))) {
    } else if (!is_add && need_conflict && OB_FAIL(conflict_stack.push_back(iter))) {
    } else {
      // no add or push back over
      iter++;
    }
  }

  return ret;
}

int ObIKArbitrator::remove_conflict(const ObIKToken &token, ObIKTokenChain *option)
{
  int ret = OB_SUCCESS;
  ObIKToken tmp;
  while (OB_SUCC(ret) && option->check_conflict(token)) {
    if (OB_FAIL(option->pop_back(tmp))) {
    }
  }
  return ret;
}

int ObIKArbitrator::prepare(TokenizeContext &ctx)
{
  int ret = OB_SUCCESS;

  // Every document starts from an empty chain map.  The arbitrator is reused
  // across the documents of a thread, so the previous document has to be
  // dropped first: clear the entries and rewind the arena that holds the chains.
  reuse();

  int cal_bucket_num = MAX(ctx.fulltext_len() / 100, 10);
  cal_bucket_num = MIN(cal_bucket_num, 100);
  if (OB_LIKELY(cal_bucket_num == bucket_num_)) {
    // Same document size class, the bucket array can be kept.
  } else if (OB_UNLIKELY(-1 != bucket_num_) && OB_FAIL(chains_.destroy())) {
  } else if (OB_FAIL(chains_.create(cal_bucket_num, ObMemAttr("IK ARBITRATE")))) {
    bucket_num_ = -1;
  } else {
    bucket_num_ = cal_bucket_num;
  }
  return ret;
}

// A document leaves a few kilobytes in the arena of the arbitrator.  The pages
// are kept for the next document of the thread, but a single very large document
// must not pin its memory until the thread exits.
constexpr int64_t IK_ARBITRATOR_KEEP_MAX_BYTES = 512 * 1024;

void ObIKArbitrator::reuse()
{
  chain_count_ = 0;
  chain_order_ok_ = true;
  if (-1 != bucket_num_) {
    // The entries point into the arena that is rewound right below, so the map
    // has to be emptied first.  clear() keeps the bucket array.
    chains_.reuse();
  }
  if (alloc_.used() > IK_ARBITRATOR_KEEP_MAX_BYTES) {
    alloc_.reset();
  } else {
    alloc_.reuse();
  }
}

ObIKArbitrator::ObIKArbitrator()
  : alloc_(lib::ObMemAttr("IK Arbitrator")), chain_count_(0), chain_order_ok_(true), bucket_num_(-1)
{
}

int ObIKArbitrator::add_chain(ObIKTokenChain *chain)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(chain)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (chain->list().is_empty()) {
    // no need to add empty chain
  } else if (OB_FAIL(chains_.set_refactored(chain->min_offset(), chain))) {
  } else if (chain_count_ < MAX_ORDERED_CHAINS
             && (0 == chain_count_
                 || chain->min_offset() > chain_order_[chain_count_ - 1].offset_)) {
    chain_order_[chain_count_].offset_ = chain->min_offset();
    chain_order_[chain_count_].chain_ = chain;
    ++chain_count_;
  } else {
    // Duplicate or unordered offset, or too many chains: use the map.
    chain_order_ok_ = false;
  }
  return ret;
}

int ObIKArbitrator::lookup_chain(const int64_t offset, ObIKTokenChain *&chain) const
{
  int ret = OB_HASH_NOT_EXIST;
  chain = nullptr;
  if (OB_LIKELY(chain_order_ok_)) {
    int64_t low = 0;
    int64_t high = chain_count_ - 1;
    while (low <= high) {
      const int64_t mid = (low + high) / 2;
      if (chain_order_[mid].offset_ == offset) {
        chain = chain_order_[mid].chain_;
        ret = OB_SUCCESS;
        break;
      } else if (chain_order_[mid].offset_ < offset) {
        low = mid + 1;
      } else {
        high = mid - 1;
      }
    }
  } else {
    ret = chains_.get_refactored(offset, chain);
  }
  return ret;
}

ObIKArbitrator::~ObIKArbitrator()
{
  chains_.destroy();
  alloc_.reset();
}
} // namespace storage
} // namespace oceanbase
