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

#include "object/ob_object.h"
#define USING_LOG_PREFIX STORAGE_FTS

#include "share/rc/ob_server_runtime.h"
#include "storage/fts/ob_fts_stop_word.h"
#include "storage/fts/ob_fts_parser_helper.h"
#include "storage/fts/ob_fts_parser_property.h"

namespace oceanbase
{
namespace storage
{

////////////////////////////////////////////////////////////////////////////////
// class ObStopWordChecker
ObStopWordChecker::~ObStopWordChecker()
{
  destroy();
}

int ObStopWordChecker::init()
{
  int ret = OB_SUCCESS;
  
  

  if (inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(stopword_set_.create(DEFAULT_STOPWORD_BUCKET_NUM, "StopWordSet", "StopWordSet"))) {
  } else {
    ObObjMeta stop_meta;
    stop_meta.set_varchar();
    stop_meta.set_collation_type(ObCollationType::CS_TYPE_UTF8MB4_GENERAL_CI);

    stopword_type_.set_meta(stop_meta);
    const int64_t stopword_count = sizeof(ob_stop_word_list) / sizeof(ob_stop_word_list[0]);
    bool all_ascii = true;
    for (int64_t i = 0; OB_SUCC(ret) && i < stopword_count; ++i) {
      const char *stopword_str = ob_stop_word_list[i];
      const int64_t stopword_len = STRLEN(stopword_str);
      for (int64_t j = 0; all_ascii && j < stopword_len; ++j) {
        all_ascii = (static_cast<unsigned char>(stopword_str[j]) < 0x80);
      }
      ObFTWord stopword(stopword_len, stopword_str, stopword_type_);
      if (OB_FAIL(stopword_set_.set_refactored(stopword))) {
      }
    }
    if (OB_SUCC(ret)) {
      stopwords_all_ascii_ = all_ascii;
    }

    if (OB_SUCC(ret)) {
      inited_ = true;
    }
  }
  return ret;
}

void ObStopWordChecker::destroy()
{
  if (inited_) {
    stopword_set_.destroy();
    stopwords_all_ascii_ = false;
    inited_ = false;
  }
}

int ObStopWordChecker::check_stopword(const ObFTWord &word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  is_stopword = false;
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (word.get_collation_type() == stopword_type_.get_collation_type()) {
    // Same collation: the comparison can use the token bytes directly.  This is
    // the common case and avoids the per-token allocator plus charset
    // conversion that the general path needs.
    const common::ObString word_str = word.get_word().get_string();
    bool may_match = true;
    if (stopwords_all_ascii_) {
      for (int64_t i = 0; may_match && i < word_str.length(); ++i) {
        may_match = (static_cast<unsigned char>(word_str.ptr()[i]) < 0x80);
      }
    }
    if (!may_match) {
      is_stopword = false;
    } else {
      ObFTWord converted(word_str.length(), word_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  } else {
    common::ObArenaAllocator allocator(lib::ObMemAttr("ChkStopWord"));
    common::ObString cmp_str;
    if (OB_FAIL(common::ObCharset::charset_convert(
                                       allocator,
                                       word.get_word().get_string(),
                                       word.get_collation_type(),
                                       stopword_type_.get_collation_type(),
                                       cmp_str))) {
    } else {
      ObFTWord converted(cmp_str.length(), cmp_str.ptr(), stopword_type_);
      ret = stopword_set_.exist_refactored(converted);
      if (OB_HASH_NOT_EXIST == ret) {
        is_stopword = false;
        ret = OB_SUCCESS;
      } else if (OB_HASH_EXIST == ret) {
        is_stopword = true;
        ret = OB_SUCCESS;
      } else if (OB_SUCC(ret)) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////////////////////
// class ObAddWord
ObAddWord::ObAddWord(
    const ObFTParserProperty &property,
    const ObObjMeta &meta,
    const ObAddWordFlag &flag,
    common::ObIAllocator &allocator,
    ObFTWordMap &word_map)
  : word_meta_(meta),
    allocator_(allocator),
    word_map_(word_map),
    min_max_word_cnt_(0),
    non_stopword_cnt_(0),
    stopword_cnt_(0),
    min_token_size_(property.min_token_size_),
    max_token_size_(property.max_token_size_),
    flag_(flag)
{
}

int ObAddWord::process_word(
    const char *word,
    const int64_t word_len,
    const int64_t char_cnt,
    const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  bool is_stopword = false;
  ObFTWord src_word(word_len, word, word_meta_);
  ObFTWord dst_word;
  if (OB_ISNULL(word) || OB_UNLIKELY(0 >= word_len || 0 >= char_cnt || 0 >= word_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (is_min_max_word(char_cnt)) {
    ++min_max_word_cnt_;
  } else if (OB_FAIL(casedown_word(src_word, dst_word))) {
  } else if (OB_FAIL(check_stopword(dst_word, is_stopword))) {
  } else if (OB_UNLIKELY(is_stopword)) {
    ++stopword_cnt_;
  } else if (OB_FAIL(groupby_word(dst_word, word_freq))) {
  } else {
    non_stopword_cnt_ += word_freq;
  }
  return ret;
}

bool ObAddWord::is_min_max_word(const int64_t c_len) const
{
  return flag_.min_max_word() && (c_len < min_token_size_ || c_len > max_token_size_);
}

namespace
{
// Characters that have no lowercase mapping in any of the UTF8MB4 collations.
// They are the bulk of a Chinese document, and lowercasing them is a
// byte-for-byte copy: a token made only of such characters can be used in place
// without allocating and scanning a converted copy.  Anything that is not
// listed here (including malformed input) is treated as possibly cased and goes
// through the regular conversion, so the result never changes.
bool is_caseless_char(const uint32_t cp)
{
  return (cp >= 0x3000 && cp <= 0x303F)     // CJK symbols and punctuation
         || (cp >= 0x3040 && cp <= 0x30FF)  // Hiragana and Katakana
         || (cp >= 0x3400 && cp <= 0x4DBF)  // CJK unified ideographs ext. A
         || (cp >= 0x4E00 && cp <= 0x9FFF)  // CJK unified ideographs
         || (cp >= 0xAC00 && cp <= 0xD7A3)  // Hangul syllables
         || (cp >= 0xF900 && cp <= 0xFAFF)  // CJK compatibility ideographs
         // Fullwidth forms, except the letters (0xFF21~0xFF3A, 0xFF41~0xFF5A).
         || (cp >= 0xFF01 && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40)
         || (cp >= 0xFF5B && cp <= 0xFF65) || (cp >= 0xFFE0 && cp <= 0xFFE6)
         || (cp >= 0x1F300 && cp <= 0x1FAFF)  // emoji and pictographs
         || (cp >= 0x20000 && cp <= 0x2FA1F); // CJK ideographs ext. B and later
}
} // namespace

bool ObAddWord::needs_case_conversion(const common::ObString &word)
{
  const unsigned char *ptr = reinterpret_cast<const unsigned char *>(word.ptr());
  const int64_t len = word.length();
  bool needs = false;
  for (int64_t i = 0; !needs && i < len;) {
    const unsigned char c = ptr[i];
    if (c < 0x80) {
      // Only A-Z fold.
      needs = (c >= 'A' && c <= 'Z');
      ++i;
    } else if (c < 0xC2) {
      // Continuation byte or overlong encoding: let the charset decide.
      needs = true;
    } else if (c < 0xE0) {
      // Two byte sequences are Latin/Greek/Cyrillic letters and friends.
      needs = true;
    } else if (c < 0xF0) {
      if (i + 2 >= len || (ptr[i + 1] & 0xC0) != 0x80 || (ptr[i + 2] & 0xC0) != 0x80) {
        needs = true;
      } else {
        const uint32_t cp = ((c & 0x0F) << 12) | ((ptr[i + 1] & 0x3F) << 6)
                            | (ptr[i + 2] & 0x3F);
        needs = !is_caseless_char(cp);
        i += 3;
      }
    } else if (c < 0xF5) {
      if (i + 3 >= len || (ptr[i + 1] & 0xC0) != 0x80 || (ptr[i + 2] & 0xC0) != 0x80
          || (ptr[i + 3] & 0xC0) != 0x80) {
        needs = true;
      } else {
        const uint32_t cp = ((c & 0x07) << 18) | ((ptr[i + 1] & 0x3F) << 12)
                            | ((ptr[i + 2] & 0x3F) << 6) | (ptr[i + 3] & 0x3F);
        needs = !is_caseless_char(cp);
        i += 4;
      }
    } else {
      needs = true;
    }
  }
  return needs;
}

int ObAddWord::casedown_word(const ObFTWord &src, ObFTWord &dst)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(src.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.casedown()) {
    const ObString src_str = src.get_word().get_string();
    if (!needs_case_conversion(src_str)) {
      // Lowercasing would be a byte-for-byte copy, keep the token in place and
      // skip the per-token allocation.
      dst = src;
    } else {
      ObString dst_str;
      if (OB_FAIL(ObCharset::tolower(
                      word_meta_.get_collation_type(),
                      src_str,
                      dst_str,
                      allocator_))) {
      } else {
        ObFTWord tmp(dst_str.length(), dst_str.ptr(), word_meta_);
        dst = tmp;
      }
    }
  } else {
    dst = src;
  }
  return ret;
}

int ObAddWord::check_stopword(const ObFTWord &ft_word, bool &is_stopword)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(ft_word.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (flag_.stopword()) {
    ObStopWordChecker *stop_word_checker = ObFTParseData::instance().stop_word_checker();
    if (OB_ISNULL(stop_word_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(stop_word_checker->check_stopword(ft_word, is_stopword))) {
    }
  }
  return ret;
}

namespace {
// Folds one more occurrence of a token into its existing counter.  Using
// set_or_update keeps the node and updates the value in place, while the
// get/set pair used to re-hash the key and rebuild the node on every hit.
struct ObFTWordCountAccumulator
{
  explicit ObFTWordCountAccumulator(const int64_t word_freq) : word_freq_(word_freq) {}
  template <class PairType>
  void operator()(PairType &pair) { pair.second += word_freq_; }
  int64_t word_freq_;
};
} // namespace

int ObAddWord::groupby_word(const ObFTWord &word, const int64_t word_freq)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(word.empty() || word_freq <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (!flag_.groupby_word()) {
    if (OB_FAIL(word_map_.set_refactored(word, 1/*word count*/))) {
    }
  } else {
    // New tokens start at one, later occurrences add their frequency, which is
    // the same accumulation the explicit lookup used to perform.
    ObFTWordCountAccumulator accumulator(word_freq);
    if (OB_FAIL(word_map_.set_or_update(word, 1/*initial word count*/, accumulator))) {
    }
  }
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
