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

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "lib/alloc/alloc_struct.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ik_dic.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{

namespace
{
// Byte length of a UTF8 character from its leading byte.  The IK dictionary is
// valid UTF8 (it is a compile time table), so no validation is needed here; a
// stray invalid byte is treated as a one byte character and simply never
// matches, which is the same thing the dictionary build would do.
OB_INLINE uint8_t utf8_char_len(const uint8_t lead)
{
  if (lead < 0x80) {
    return 1;
  } else if (lead < 0xE0) {
    return 2;
  } else if (lead < 0xF0) {
    return 3;
  } else {
    return 4;
  }
}

// Inverse of IKFastDict::pack_key(): the code point of a packed character.
OB_INLINE uint32_t unpack_codepoint(const uint32_t key)
{
  const uint32_t b0 = key >> 24;
  uint32_t cp = 0;
  if (b0 < 0x80) {
    cp = b0;
  } else if (b0 < 0xE0) {
    cp = ((b0 & 0x1F) << 6) | ((key >> 16) & 0x3F);
  } else if (b0 < 0xF0) {
    cp = ((b0 & 0x0F) << 12) | (((key >> 16) & 0x3F) << 6) | ((key >> 8) & 0x3F);
  } else {
    // Four byte character: never looked up in the direct table.
    cp = IKFastDict::ROOT_TABLE_SIZE;
  }
  return cp;
}
} // namespace

int IKFastDict::build()
{
  int ret = OB_SUCCESS;
  const ObIKDictLoader::RawDict raw = ObIKDictLoader::dict_text();
  const char *const *words = raw.data_;
  const int64_t word_num = raw.array_size_;

  // Temporary build state.  Node 0 is the root; every other node records its
  // parent and the packed key of the edge that reaches it.  Because the word
  // list is sorted by bytes, the children of a node are created in increasing
  // key order and a child that already exists is always the last one created
  // for that node, so no lookup is needed while building.
  std::vector<uint32_t> parent;
  std::vector<uint32_t> edge_key;
  std::vector<uint8_t> is_word;
  std::vector<uint32_t> last_child;
  const uint32_t NO_NODE = 0xFFFFFFFFu;

  if (OB_ISNULL(words) || OB_UNLIKELY(word_num <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    // About 4.2e5 nodes for the current dictionary; reserving up front keeps the
    // build free of reallocations.
    parent.reserve(1 << 19);
    edge_key.reserve(1 << 19);
    is_word.reserve(1 << 19);
    last_child.reserve(1 << 19);
    parent.push_back(0);
    edge_key.push_back(0);
    is_word.push_back(0);
    last_child.push_back(NO_NODE);

    for (int64_t i = 0; OB_SUCC(ret) && i < word_num; ++i) {
      const char *word = words[i];
      if (OB_ISNULL(word)) {
        ret = OB_ERR_UNEXPECTED;
        break;
      }
      const int64_t word_len = static_cast<int64_t>(strlen(word));
      uint32_t cur = 0;
      int64_t offset = 0;
      while (OB_SUCC(ret) && offset < word_len) {
        const uint8_t char_len = utf8_char_len(static_cast<uint8_t>(word[offset]));
        if (OB_UNLIKELY(offset + char_len > word_len)) {
          ret = OB_ERR_UNEXPECTED; // malformed dictionary entry
          break;
        }
        const uint32_t key = IKFastDict::pack_key(word + offset, char_len);
        uint32_t next = NO_NODE;
        if (last_child[cur] != NO_NODE && edge_key[last_child[cur]] == key) {
          next = last_child[cur];
        } else {
          next = static_cast<uint32_t>(parent.size());
          parent.push_back(cur);
          edge_key.push_back(key);
          is_word.push_back(0);
          last_child.push_back(NO_NODE);
          last_child[cur] = next;
        }
        cur = next;
        offset += char_len;
      }
      if (OB_SUCC(ret)) {
        is_word[cur] = 1;
      }
    }
  }

  if (OB_SUCC(ret)) {
    const uint32_t node_num = static_cast<uint32_t>(parent.size());
    // Prefix sums of the child counts give the children offset of every node;
    // nodes_ gets one extra entry holding the total, so that the end of the
    // children of node i is the offset stored in nodes_[i + 1].
    std::vector<uint32_t> nodes(node_num + 1, 0);
    std::vector<uint32_t> cursor(node_num, 0);
    uint32_t offset = 0;
    // Count the children of every node first, then turn the counts into offsets.
    std::vector<uint32_t> child_cnt(node_num, 0);
    for (uint32_t i = 1; i < node_num; ++i) {
      ++child_cnt[parent[i]];
    }
    offset = 0;
    for (uint32_t i = 0; i < node_num; ++i) {
      nodes[i] = (is_word[i] ? WORD_FLAG : 0) | offset;
      cursor[i] = offset;
      offset += child_cnt[i];
    }
    nodes[node_num] = offset;
    const uint32_t kid_num = offset;

    std::vector<IKFastDict::Kid> kids(kid_num);
    for (uint32_t i = 1; i < node_num; ++i) {
      const uint32_t p = parent[i];
      kids[cursor[p]].key_ = edge_key[i];
      kids[cursor[p]].child_ = i;
      ++cursor[p];
    }

    // First level direct table, indexed by code point.
    std::vector<uint32_t> root_table(ROOT_TABLE_SIZE, 0);
    for (uint32_t i = 1; i < node_num; ++i) {
      if (0 == parent[i]) {
        const uint32_t cp = unpack_codepoint(edge_key[i]);
        if (cp < ROOT_TABLE_SIZE) {
          root_table[cp] = i;
        }
      }
    }

    // One immutable block owns the three arrays for the whole process.
    const size_t nodes_bytes = nodes.size() * sizeof(uint32_t);
    const size_t kids_bytes = kids.size() * sizeof(IKFastDict::Kid);
    const size_t root_bytes = root_table.size() * sizeof(uint32_t);
    char *block = static_cast<char *>(common::ob_malloc(
        nodes_bytes + kids_bytes + root_bytes, lib::ObMemAttr("IKFastDict")));
    if (OB_ISNULL(block)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(block, nodes.data(), nodes_bytes);
      MEMCPY(block + nodes_bytes, kids.data(), kids_bytes);
      MEMCPY(block + nodes_bytes + kids_bytes, root_table.data(), root_bytes);
      nodes_ = reinterpret_cast<const uint32_t *>(block);
      kids_ = reinterpret_cast<const Kid *>(block + nodes_bytes);
      root_table_ = reinterpret_cast<const uint32_t *>(block + nodes_bytes + kids_bytes);
      node_count_ = node_num;
      kid_count_ = kid_num;
      LOG_INFO("built the compact IK dictionary trie", K(node_num), K(kid_num), K(nodes_bytes),
               K(kids_bytes), K(root_bytes));
    }
  }
  return ret;
}

uint32_t IKFastDict::root_child_slow(const uint32_t key) const
{
  uint32_t begin = nodes_[0] & OFF_MASK;
  uint32_t end = nodes_[1] & OFF_MASK;
  while (begin < end) {
    const uint32_t mid = (begin + end) >> 1;
    if (kids_[mid].key_ < key) {
      begin = mid + 1;
    } else {
      end = mid;
    }
  }
  const uint32_t last = nodes_[1] & OFF_MASK;
  return (begin < last && kids_[begin].key_ == key) ? kids_[begin].child_ : 0;
}

const IKFastDict *IKFastDict::get()
{
  // Built once, never destroyed: it is read only for the lifetime of the
  // process and destroying it at exit would race with the shutdown order of the
  // allocator.  A failed build leaves the pointer null and the caller keeps the
  // double array trie path.
  static const IKFastDict *instance = []() -> const IKFastDict * {
    IKFastDict *dict = new (std::nothrow) IKFastDict();
    if (nullptr != dict && OB_SUCCESS != dict->build()) {
      // IKFastDict has no public destructor; the build failure path leaks only
      // the object itself (the arrays are allocated last).
      dict = nullptr;
    }
    return dict;
  }();
  return instance;
}

bool IKFastDict::enabled()
{
  static const bool enabled = []() -> bool {
    const char *value = ::getenv("OB_FTS_IK_FAST_INDEX");
    return !(nullptr != value && '0' == value[0]);
  }();
  return enabled;
}

int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  return OB_NOT_NULL(fast_dict_) ? do_process_fast(ctx, ch, char_len, type)
                                 : do_process_dat(ctx, ch, char_len, type);
}

int ObIKCJKProcessor::do_process_dat(TokenizeContext &ctx,
                                     const char *ch,
                                     const uint8_t char_len,
                                     const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    // handle previous hits_ first and then check from this char
    for (int64_t i = 0; OB_SUCC(ret) && i < hits_.size();) {
      ObDATrieHit &hit = hits_.at(i);
      if (OB_FAIL(dict_main_.match_with_hit({char_len, ch}, hit, hit))) {
      } else if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else if (hit.is_prefix()) {
          // match will record the start_cursor
        }
        ++i;
      } else if (hit.is_prefix()) {
        ++i;
      } else if (hit.is_unmatch()) {
        if (OB_FAIL(hits_.erase_at(i))) {
        }
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match({char_len, ch}, hit))) {
      } else if (hit.is_match()) {
        // output token
        hits_.push_back(hit);
        ctx.add_token(ctx.fulltext(),
                      ctx.get_cursor(),
                      char_len,
                      1,
                      ObIKTokenType::IK_CHINESE_TOKEN);
      } else if (hit.is_prefix()) {
        hits_.push_back(hit);
      } else {
        // ignore mismatch
      }
    }
  } else {
    // stop previous match
    hits_.clear();
  }

  if (OB_SUCC(ret)) {
    if (ctx.is_last()) {
      hits_.clear();
    }
  } else {
    hits_.clear();
  }

  return ret;
}

int ObIKCJKProcessor::do_process_fast(TokenizeContext &ctx,
                                      const char *ch,
                                      const uint8_t char_len,
                                      const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;
  const IKFastDict &dict = *fast_dict_;

  // The tokens are emitted in exactly the order the double array trie path
  // emits them: first the pending matches, oldest start first, and then the
  // match that starts at the current character.  Dropping a pending match as
  // soon as it has no children cannot skip a token (such a match can never
  // produce one) and does not reorder the remaining ones.
  if (ObFTCharUtil::CharType::USELESS != type) {
    for (int64_t i = 0; OB_SUCC(ret) && i < fast_hits_.size();) {
      IKFastHitArray::Hit &hit = fast_hits_.at(i);
      const uint32_t next = dict.child(hit.node_, ch, char_len);
      if (0 == next) {
        if (OB_FAIL(fast_hits_.erase_at(i))) {
        }
      } else {
        hit.node_ = next;
        ++hit.char_cnt_;
        if (dict.is_word(next)) {
          if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                    hit.start_,
                                    ctx.get_cursor() + char_len - hit.start_,
                                    hit.char_cnt_,
                                    ObIKTokenType::IK_CHINESE_TOKEN))) {
          }
        }
        if (OB_SUCC(ret)) {
          if (dict.has_child(next)) {
            ++i;
          } else if (OB_FAIL(fast_hits_.erase_at(i))) {
          }
        }
      }
    }
    if (OB_SUCC(ret)) {
      const uint32_t node = dict.root_child(ch, char_len);
      if (0 != node) {
        if (dict.is_word(node)) {
          if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                    ctx.get_cursor(),
                                    char_len,
                                    1,
                                    ObIKTokenType::IK_CHINESE_TOKEN))) {
          }
        }
        if (OB_SUCC(ret) && dict.has_child(node)) {
          IKFastHitArray::Hit hit;
          hit.start_ = ctx.get_cursor();
          hit.node_ = node;
          hit.char_cnt_ = 1;
          if (OB_FAIL(fast_hits_.push_back(hit))) {
          }
        }
      }
    }
  } else {
    // stop previous match
    fast_hits_.clear();
  }

  if (OB_FAIL(ret) || ctx.is_last()) {
    fast_hits_.clear();
  }

  return ret;
}

} //  namespace storage
} //  namespace oceanbase
