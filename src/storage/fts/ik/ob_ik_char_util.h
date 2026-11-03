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

#ifndef _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_
#define _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_

#include "lib/charset/ob_charset.h"
#include "lib/charset/ob_charset_string_helper.h"
#include "lib/charset/ob_ctype.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/utils/unicode_utils.h"

#include <array>
#include <cstdint>
#include <type_traits>

namespace oceanbase
{
namespace storage
{
// See: lang/Character.java
class ObFTCharUtil
{
public:
  enum class CharType : int8_t
  {
    USELESS = 0,
    ARABIC_LETTER = 1,
    ENGLISH_LETTER = 2,
    CHINESE = 3,
    OTHER_CJK = 4,
    SURROGATE_HIGH = 5,
    SURROGATE_LOW = 6,
  };

  static int classify_first_char(ObCollationType coll_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 CharType &type);

  // Same as above but takes the charset type directly, so that the hot path
  // does not resolve collation to charset once per character.
  static int classify_first_char(ObCharsetType cs_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 CharType &type);

  // Per character fast path of the UTF8MB4 parsers: a single decode yields both
  // the byte length of the first character and its CharType, instead of decoding
  // it once more to validate the length (well_formed_len) and once to classify
  // it.  The returned pair is exactly the one produced by
  // first_valid_char_at() followed by classify_first_char().
  //
  // Returns false when the bytes at `input` are not a well formed character.
  // The caller must then fall back to the regular two step path: malformed
  // input has to keep its previous diagnostics and error code, and this fast
  // path deliberately makes no attempt at interpreting it.
  static bool classify_first_valid_utf8mb4_char(const char *input,
                                                const int64_t remain,
                                                int64_t &char_len,
                                                CharType &type);

  static int check_cn_number(ObCollationType coll_type,
                             const char *input,
                             const uint8_t char_len,
                             bool &is_cn_number);

  static int check_num_connector(ObCollationType coll_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 bool &is_connector);

  static int check_letter_connector(ObCollationType coll_type,
                                    const char *input,
                                    const uint8_t char_len,
                                    bool &is_connector);

  // some cjk word should be ignored
  static int is_ignore_single_cjk(ObCollationType coll_type,
                                  const char *input,
                                  const uint8_t char_len,
                                  bool &ignore);

  // Same as above, but take the charset type directly so that the per character
  // hot path does not resolve the collation to a charset on every call.
  static int check_cn_number(ObCharsetType cs_type,
                             const char *input,
                             const uint8_t char_len,
                             bool &is_cn_number);
  static int check_num_connector(ObCharsetType cs_type,
                                 const char *input,
                                 const uint8_t char_len,
                                 bool &is_connector);
  static int check_letter_connector(ObCharsetType cs_type,
                                    const char *input,
                                    const uint8_t char_len,
                                    bool &is_connector);
  static int is_ignore_single_cjk(ObCharsetType cs_type,
                                  const char *input,
                                  const uint8_t char_len,
                                  bool &ignore);

private:
  // ASCII code points are resolved with a table: below 0x80 the unicode block
  // predicates can only report a letter or a digit, everything else is USELESS.
  // This keeps the hot path of the plain ASCII parsers free of the predicate
  // calls.  The table itself is defined after the class, see
  // ob_ik_char_util_private::ASCII_CHAR_TYPES.

  template <ObCharsetType CS_TYPE>
  static int do_classify(const char *input, const uint8_t char_len, CharType &type);

  // Classifies an already decoded code point.  The large blocks are tested with
  // a single range comparison and the unicode helpers are only reached for code
  // points outside of them.  Every special cased block is disjoint from the
  // predicates that the general path would evaluate first, so the result is the
  // same as testing all predicates in the original order.
  template <ObCharsetType CS_TYPE>
  static void classify_unicode(const ob_wc_t unicode, CharType &type);

  // The classification predicates only depend on the code point, so the
  // character is decoded once and then tested in the original order. Surrogate
  // checks only exist for UTF16 charsets: for every other charset the condition
  // is a compile time constant, so the branch and the call fold away.
  template <ObCharsetType CS_TYPE>
  static bool is_surrogate_high_unicode(const ob_wc_t unicode)
  {
    return (CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE)
           && ObUnicodeBlockUtils::check_high_surrogate(unicode);
  }

  template <ObCharsetType CS_TYPE>
  static bool is_surrogate_low_unicode(const ob_wc_t unicode)
  {
    return (CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE)
           && ObUnicodeBlockUtils::check_low_surrogate(unicode);
  }

private:
  /*************************  Alpha: english letter abc *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_alpha(const char *input, const uint8_t char_len, bool &is_alpha);
  /****************************************************************************************/

  /*************************  Arabic: num letter 123 *********************/

  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_arabic(const char *input, const uint8_t char_len, bool &is_arabic);
  /****************************************************************************************/

  /*************************  Chinese: Characters *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_chinese(const char *input, const uint8_t char_len, bool &is_chinese);
  /****************************************************************************************/

  /*************************  Other CJK:  Japanese Korean Chinese *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_other_cjk(const char *input, const uint8_t char_len, bool &is_other_cjk);
  /****************************************************************************************/

  /*************************  Surrogate: only for UTF16 *********************/
  template <ObCharsetType CS_TYPE>
  static
      typename std::enable_if<(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
      is_surrogate_high(const char *input, const uint8_t char_len, bool &is_surrogate_high);

  template <ObCharsetType CS_TYPE>
  static
      typename std::enable_if<(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
      is_surrogate_low(const char *input, const uint8_t char_len, bool &is_surrogate_low);

  template <ObCharsetType CS_TYPE>
  static
      typename std::enable_if<!(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
      is_surrogate_high(const char *input, const uint8_t char_len, bool &is_surrogate_high);

  template <ObCharsetType CS_TYPE>
  static
      typename std::enable_if<!(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
      is_surrogate_low(const char *input, const uint8_t char_len, bool &is_surrogate_low);
  /****************************************************************************************/

  /*************************  Ignore: some char should not output itself *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_ignore(const char *input, const uint8_t char_len, bool &ignore);
  /****************************************************************************************/

  /*************************  CN Number: one two three two *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_cn_number(const char *input, const uint8_t char_len, bool &is_cn_number);
  /****************************************************************************************/

  /*************************  English letter connector: #,-@ *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_letter_connector(const char *input, const uint8_t char_len, bool &is_connector);
  /****************************************************************************************/

  /*************************  Number letter connector: ,. *********************/
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  is_num_connector(const char *input, const uint8_t char_len, bool &is_connector);
  /****************************************************************************************/

private:
  // only for unicode charset
  template <ObCharsetType CS_TYPE>
  static typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                  || CS_TYPE == CHARSET_UTF16LE),
                                 int>::type
  decode_unicode(const char *input, const uint8_t char_len, ob_wc_t &unicode)
  {
    int ret = OB_SUCCESS;
    if (OB_ISNULL(input)) {
      ret = OB_INVALID_ARGUMENT;
      STORAGE_FTS_LOG(WARN, "Invalid input", K(input), K(ret));
    } else {
      const unsigned char *ustart = reinterpret_cast<const unsigned char *>(input);
      const unsigned char *uend = reinterpret_cast<const unsigned char *>(&input[char_len]);
      int code_size = common::ob_charset_decode_unicode<CS_TYPE>(ustart, uend, unicode);
      if (code_size < 0) {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
        STORAGE_FTS_LOG(WARN, "Failed to decode unicode", K(code_size), K(ret));
      }
    }
    return ret;
  }
}; // End of class.

namespace ob_ik_char_util_private
{
// ASCII -> CharType, initialized at compile time.  Only the letters and the
// digits are not USELESS below 0x80, which is exactly what the unicode block
// predicates report for those code points.
constexpr std::array<ObFTCharUtil::CharType, 128> make_ascii_char_types()
{
  std::array<ObFTCharUtil::CharType, 128> types {};
  for (int32_t i = 'A'; i <= 'Z'; ++i) {
    types[i] = ObFTCharUtil::CharType::ENGLISH_LETTER;
  }
  for (int32_t i = 'a'; i <= 'z'; ++i) {
    types[i] = ObFTCharUtil::CharType::ENGLISH_LETTER;
  }
  for (int32_t i = '0'; i <= '9'; ++i) {
    types[i] = ObFTCharUtil::CharType::ARABIC_LETTER;
  }
  return types;
}
inline constexpr std::array<ObFTCharUtil::CharType, 128> ASCII_CHAR_TYPES
    = make_ascii_char_types();
} // namespace ob_ik_char_util_private

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_alpha(const char *input, const uint8_t char_len, bool &is_alpha)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_alpha = ObUnicodeBlockUtils::is_alpha(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_arabic(const char *input, const uint8_t char_len, bool &is_arabic)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_arabic = ObUnicodeBlockUtils::is_arabic(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_chinese(const char *input, const uint8_t char_len, bool &is_chinese)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_chinese = ObUnicodeBlockUtils::is_chinese(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_other_cjk(const char *input, const uint8_t char_len, bool &is_other_cjk)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_other_cjk = ObUnicodeBlockUtils::is_other_cjk(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
ObFTCharUtil::is_surrogate_high(const char *input, const uint8_t char_len, bool &is_surrogate_high)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_surrogate_high = ObUnicodeBlockUtils::check_high_surrogate(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
ObFTCharUtil::is_surrogate_low(const char *input, const uint8_t char_len, bool &is_surrogate_low)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_surrogate_low = ObUnicodeBlockUtils::check_low_surrogate(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<!(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
ObFTCharUtil::is_surrogate_high(const char *input, const uint8_t char_len, bool &is_surrogate_high)
{
  int ret = OB_SUCCESS;
  is_surrogate_high = false;
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<!(CS_TYPE == CHARSET_UTF16 || CS_TYPE == CHARSET_UTF16LE), int>::type
ObFTCharUtil::is_surrogate_low(const char *input, const uint8_t char_len, bool &is_surrogate_low)
{
  int ret = OB_SUCCESS;
  is_surrogate_low = false;
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_ignore(const char *input, const uint8_t char_len, bool &ignore)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    ignore = ObUnicodeBlockUtils::check_ignore_as_single(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_cn_number(const char *input, const uint8_t char_len, bool &is_cn_number)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_cn_number = ObUnicodeBlockUtils::is_unicode_cn_number(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_letter_connector(const char *input, const uint8_t char_len, bool &is_connector)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_connector = ObUnicodeBlockUtils::check_letter_connector(unicode);
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline typename std::enable_if<(CS_TYPE == CHARSET_UTF8MB4 || CS_TYPE == CHARSET_UTF16
                                || CS_TYPE == CHARSET_UTF16LE),
                               int>::type
ObFTCharUtil::is_num_connector(const char *input, const uint8_t char_len, bool &is_connector)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    is_connector = ObUnicodeBlockUtils::check_num_connector(unicode);
  }
  return ret;
}

// Support GBK

// Implementation of some frame;

inline int ObFTCharUtil::check_cn_number(ObCollationType coll_type,
                                         const char *input,
                                         const uint8_t char_len,
                                         bool &is_cn_number)
{
  return check_cn_number(ObCharset::charset_type_by_coll(coll_type), input, char_len, is_cn_number);
}

inline int ObFTCharUtil::check_cn_number(ObCharsetType cs_type,
                                         const char *input,
                                         const uint8_t char_len,
                                         bool &is_cn_number)
{
  int ret = OB_SUCCESS;
  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_cn_number<CHARSET_UTF8MB4>(input, char_len, is_cn_number);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_num_connector(ObCollationType coll_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             bool &is_connector)
{
  return check_num_connector(
      ObCharset::charset_type_by_coll(coll_type), input, char_len, is_connector);
}

inline int ObFTCharUtil::check_num_connector(ObCharsetType cs_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             bool &is_connector)
{
  int ret = OB_SUCCESS;
  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_num_connector<CHARSET_UTF8MB4>(input, char_len, is_connector);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::check_letter_connector(ObCollationType coll_type,
                                                const char *input,
                                                const uint8_t char_len,
                                                bool &is_connector)
{
  return check_letter_connector(
      ObCharset::charset_type_by_coll(coll_type), input, char_len, is_connector);
}

inline int ObFTCharUtil::check_letter_connector(ObCharsetType cs_type,
                                                const char *input,
                                                const uint8_t char_len,
                                                bool &is_connector)
{
  int ret = OB_SUCCESS;
  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    // ret = ObFTCharUtil::do_check_letter_connector<CHARSET_UTF8MB4>(input, char_len,
    // is_connector);
    ret = ObFTCharUtil::is_letter_connector<CHARSET_UTF8MB4>(input, char_len, is_connector);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
  }
  return ret;
}

inline int ObFTCharUtil::is_ignore_single_cjk(ObCollationType coll_type,
                                              const char *input,
                                              const uint8_t char_len,
                                              bool &ignore)
{
  return is_ignore_single_cjk(
      ObCharset::charset_type_by_coll(coll_type), input, char_len, ignore);
}

inline int ObFTCharUtil::is_ignore_single_cjk(ObCharsetType cs_type,
                                              const char *input,
                                              const uint8_t char_len,
                                              bool &ignore)
{
  int ret = OB_SUCCESS;
  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = ObFTCharUtil::is_ignore<CHARSET_UTF8MB4>(input, char_len, ignore);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

template <ObCharsetType CS_TYPE>
inline void ObFTCharUtil::classify_unicode(const ob_wc_t unicode, CharType &type)
{
  type = CharType::USELESS;
  if (unicode < 0x80) {
    type = ob_ik_char_util_private::ASCII_CHAR_TYPES[unicode];
  } else if (unicode >= 0x4E00 && unicode <= 0x9FFF) {
    type = CharType::CHINESE; // CJK Unified Ideographs
  } else if (unicode >= 0x3400 && unicode <= 0x4DBF) {
    type = CharType::CHINESE; // CJK Unified Ideographs Extension A
  } else if (unicode >= 0xF900 && unicode <= 0xFAFF) {
    type = CharType::CHINESE; // CJK Compatibility Ideographs
  } else if (unicode >= 0xFF00 && unicode <= 0xFFEF) {
    // Halfwidth and fullwidth forms: the fullwidth letters and digits belong to
    // the letter classes and every other code point of the block to OTHER_CJK,
    // which is the order the predicates are evaluated in the general path.
    if (ObUnicodeBlockUtils::is_alpha(unicode)) {
      type = CharType::ENGLISH_LETTER;
    } else if (ObUnicodeBlockUtils::is_arabic(unicode)) {
      type = CharType::ARABIC_LETTER;
    } else {
      type = CharType::OTHER_CJK;
    }
  } else if (unicode >= 0xAC00 && unicode <= 0xD7AF) {
    type = CharType::OTHER_CJK; // Hangul Syllables
  } else if ((unicode >= 0x3040 && unicode <= 0x30FF) // Hiragana + Katakana
             || (unicode >= 0x31F0 && unicode <= 0x31FF) // Katakana Phonetic Extensions
             || (unicode >= 0x1100 && unicode <= 0x11FF) // Hangul Jamo
             || (unicode >= 0x3130 && unicode <= 0x318F)) { // Hangul Compatibility Jamo
    type = CharType::OTHER_CJK;
  } else if (is_surrogate_high_unicode<CS_TYPE>(unicode)) {
    type = CharType::SURROGATE_HIGH;
  } else if (is_surrogate_low_unicode<CS_TYPE>(unicode)) {
    type = CharType::SURROGATE_LOW;
  } else {
    // keep USELESS
  }
}

inline bool ObFTCharUtil::classify_first_valid_utf8mb4_char(const char *input,
                                                             const int64_t remain,
                                                             int64_t &char_len,
                                                             CharType &type)
{
  bool found = false;
  char_len = 0;
  type = CharType::USELESS;
  if (OB_LIKELY(nullptr != input && remain > 0)) {
    const unsigned char *ustart = reinterpret_cast<const unsigned char *>(input);
    ob_wc_t unicode = 0;
    const int code_size
        = common::ob_charset_decode_unicode<CHARSET_UTF8MB4>(ustart, ustart + remain, unicode);
    if (OB_LIKELY(code_size > 0)) {
      char_len = code_size;
      if (1 == code_size && static_cast<unsigned char>(input[0]) < 0x80) {
        // Same table the classification fast path uses for ASCII.
        type = ob_ik_char_util_private::ASCII_CHAR_TYPES[static_cast<unsigned char>(input[0])];
      } else {
        classify_unicode<CHARSET_UTF8MB4>(unicode, type);
      }
      found = true;
    }
  }
  return found;
}

template <ObCharsetType CS_TYPE>
inline int ObFTCharUtil::do_classify(const char *input, const uint8_t char_len, CharType &type)
{
  int ret = OB_SUCCESS;
  ob_wc_t unicode = 0;
  type = CharType::USELESS;

  if (OB_ISNULL(input)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_FTS_LOG(WARN, "Invalid input", K(input), K(ret));
  } else if (CS_TYPE == CHARSET_UTF8MB4 && 1 == char_len
             && static_cast<unsigned char>(input[0]) < 0x80) {
    // Single byte utf8 is ASCII, no decode is needed.
    type = ob_ik_char_util_private::ASCII_CHAR_TYPES[static_cast<unsigned char>(input[0])];
  } else if (OB_FAIL(decode_unicode<CS_TYPE>(input, char_len, unicode))) {
  } else {
    // Decode once: every predicate only inspects the code point, so the
    // previous per-predicate decode was pure duplicated work.
    classify_unicode<CS_TYPE>(unicode, type);
  }
  return ret;
}

inline int ObFTCharUtil::classify_first_char(ObCharsetType cs_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             CharType &type)
{
  int ret = OB_SUCCESS;

  switch (cs_type) {
  case CHARSET_UTF8MB4: {
    ret = do_classify<CHARSET_UTF8MB4>(input, char_len, type);
    break;
  }
  case CHARSET_UTF16: {
    ret = do_classify<CHARSET_UTF16>(input, char_len, type);
    break;
  }
  case CHARSET_UTF16LE: {
    ret = do_classify<CHARSET_UTF16LE>(input, char_len, type);
    break;
  }
  default:
    ret = OB_NOT_SUPPORTED;
    STORAGE_FTS_LOG(WARN, "Not supported charset type", K(ret), K(cs_type));
    break;
  }
  return ret;
}

inline int ObFTCharUtil::classify_first_char(ObCollationType coll_type,
                                             const char *input,
                                             const uint8_t char_len,
                                             CharType &type)
{
  return classify_first_char(ObCharset::charset_type_by_coll(coll_type), input, char_len, type);
}

} // namespace storage
} // namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_IK_OB_IK_CHAR_UTIL_H_
