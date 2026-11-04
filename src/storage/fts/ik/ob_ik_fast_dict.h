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

#include <cstdint>
#include <cstdlib>

#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace storage
{

/**
 * @class IKFastDict
 * @brief A compact prefix trie over the built-in IK main dictionary.
 *
 * The CJK processor asks the dictionary exactly two questions per input
 * character: "can this character continue the current word" and "does a word
 * end here".  The double array trie answers both through a per range hash map
 * (character -> code) followed by two indirections into the base/check arrays,
 * which costs a hash, a modulo and up to three independent random accesses for
 * every character of the text.
 *
 * This trie answers the same questions with the same answer for every input,
 * but with at most two loads: one into the node array (children offset plus the
 * "word ends here" bit) and one into the children array.  Characters of the
 * first level are resolved with a direct table indexed by code point.
 *
 * The trie is built once per process from ObIKDictLoader::dict_text(), which is
 * the same immutable word list the range dictionaries are built from, so the
 * accepted language is identical.  Nothing points into the dictionary cache:
 * the trie owns its nodes and compares raw character bytes.
 *
 * The class is only used when ObIKCJKProcessor is constructed with a non null
 * instance; a null instance keeps the double array trie path, which is the
 * fallback selected by OB_FTS_IK_FAST_INDEX=0.
 */
class IKFastDict final
{
public:
  struct Kid
  {
    // Up to four bytes of the character, most significant byte first, so that
    // comparing keys as integers is the same as comparing the raw bytes.
    uint32_t key_;
    uint32_t child_;
  };

  // Bit 31 of a node word marks "a dictionary word ends at this node"; the low
  // bits are the offset of the node's children in kids_.  The highest possible
  // offset is the number of edges of the IK main dictionary (about 4.2e5), far
  // below 2^31.
  static constexpr uint32_t WORD_FLAG = 0x80000000u;
  static constexpr uint32_t OFF_MASK = 0x7FFFFFFFu;

  // Code points below this bound are looked up directly on the first level.
  static constexpr uint32_t ROOT_TABLE_SIZE = 1u << 16;

public:
  // Returns the shared trie, building it on first use, or nullptr when it could
  // not be built (the caller then keeps using the double array trie).
  static const IKFastDict *get();

  // Gray release switch.  Unset or any value other than "0" enables the trie.
  static bool enabled();

public:
  // Node 0 is the root and child ids start at 1, so 0 always means "no such
  // transition" and doubles as the "not a dictionary prefix" answer.
  OB_INLINE uint32_t root_child(const char *ptr, const uint8_t len) const
  {
    // Every character of the dictionary is valid UTF8.  One to three byte
    // characters, which is all of the CJK ranges, are resolved with the direct
    // table; the (rare) four byte ones use the binary search.
    uint32_t cp = 0;
    if (OB_LIKELY(3 == len)) {
      cp = (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0]) & 0x0F) << 12)
           | (static_cast<uint32_t>(static_cast<uint8_t>(ptr[1]) & 0x3F) << 6)
           | static_cast<uint32_t>(static_cast<uint8_t>(ptr[2]) & 0x3F);
    } else if (OB_LIKELY(1 == len)) {
      cp = static_cast<uint8_t>(ptr[0]);
    } else if (OB_LIKELY(2 == len)) {
      cp = (static_cast<uint32_t>(static_cast<uint8_t>(ptr[0]) & 0x1F) << 6)
           | static_cast<uint32_t>(static_cast<uint8_t>(ptr[1]) & 0x3F);
    } else {
      cp = ROOT_TABLE_SIZE; // out of table, use the search below
    }
    return cp < ROOT_TABLE_SIZE ? root_table_[cp] : root_child_slow(pack_key(ptr, len));
  }

  OB_INLINE uint32_t child(const uint32_t node, const char *ptr, const uint8_t len) const
  {
    return child_slow(node, pack_key(ptr, len));
  }

  OB_INLINE uint32_t child_slow(const uint32_t node, const uint32_t key) const
  {
    uint32_t begin = nodes_[node] & OFF_MASK;
    const uint32_t end = nodes_[node + 1] & OFF_MASK;
    if (end - begin <= 8) {
      // The common case: a handful of children, a linear scan is the fastest.
      for (uint32_t i = begin; i < end; ++i) {
        if (kids_[i].key_ == key) {
          return kids_[i].child_;
        }
      }
      return 0;
    } else {
      uint32_t last = end;
      while (begin < last) {
        const uint32_t mid = (begin + last) >> 1;
        if (kids_[mid].key_ < key) {
          begin = mid + 1;
        } else {
          last = mid;
        }
      }
      return (begin < end && kids_[begin].key_ == key) ? kids_[begin].child_ : 0;
    }
  }

  OB_INLINE bool is_word(const uint32_t node) const { return 0 != (nodes_[node] & WORD_FLAG); }

  OB_INLINE bool has_child(const uint32_t node) const
  {
    return (nodes_[node] & OFF_MASK) != (nodes_[node + 1] & OFF_MASK);
  }

  // The node word holds both the "word ends here" bit and the children offset,
  // so one load answers both questions.
  OB_INLINE uint32_t node_word(const uint32_t node) const { return nodes_[node]; }
  OB_INLINE bool is_word_of(const uint32_t word) const { return 0 != (word & WORD_FLAG); }
  OB_INLINE bool has_child_of(const uint32_t child_begin_word, const uint32_t child_end_word) const
  {
    return (child_begin_word & OFF_MASK) != (child_end_word & OFF_MASK);
  }

public:
  // Packs the first `len` bytes of a character the same way the builder does.
  // Bytes past `len` are never read.
  static OB_INLINE uint32_t pack_key(const char *ptr, const uint8_t len)
  {
    const uint32_t b0 = static_cast<uint8_t>(ptr[0]);
    const uint32_t b1 = len > 1 ? static_cast<uint8_t>(ptr[1]) : 0;
    const uint32_t b2 = len > 2 ? static_cast<uint8_t>(ptr[2]) : 0;
    const uint32_t b3 = len > 3 ? static_cast<uint8_t>(ptr[3]) : 0;
    return (b0 << 24) | (b1 << 16) | (b2 << 8) | b3;
  }

private:
  IKFastDict() = default;
  ~IKFastDict() = default;

  int build();
  uint32_t root_child_slow(const uint32_t key) const;

private:
  // nodes_ has node_count_ + 1 entries; the last one is the end sentinel that
  // makes has_child() and child() a single subtraction.
  const uint32_t *nodes_;
  const Kid *kids_;
  const uint32_t *root_table_;
  int64_t node_count_;
  int64_t kid_count_;

private:
  DISALLOW_COPY_AND_ASSIGN(IKFastDict);
};

} // namespace storage
} // namespace oceanbase
