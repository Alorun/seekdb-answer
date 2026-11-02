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

#include "src/storage/fts/utils/unicode_utils.h"

#include "lib/charset/ob_charset_string_helper.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace storage
{
// See: java's Character.UnicodeBlock

bool ObUnicodeBlockUtils::is_alpha(const ob_wc_t unicode)
{
  bool is_ascii_alpha
      = (unicode >= 0x41 && unicode <= 0x5a) || (unicode >= 0x61 && unicode <= 0x7a);
  bool is_fw_alpha
      = (unicode >= 0xFF21 && unicode <= 0xFF3A) || (unicode >= 0xFF41 && unicode <= 0xFF5A);
  return is_ascii_alpha || is_fw_alpha;
}

bool ObUnicodeBlockUtils::is_arabic(const ob_wc_t unicode)
{
  bool is_ascii_arabic = (unicode >= 0x30 && unicode <= 0x39);
  bool is_fw_arabic = (unicode >= 0xFF10 && unicode <= 0xFF19);
  return is_ascii_arabic || is_fw_arabic;
}

bool ObUnicodeBlockUtils::check_high_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xD800 && unicode <= 0xDBFF;
}

bool ObUnicodeBlockUtils::check_high_private_use_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xDB80 && unicode <= 0xDBFF;
}

bool ObUnicodeBlockUtils::check_low_surrogate(const ob_wc_t unicode)
{
  return unicode >= 0xDC00 && unicode <= 0xDFFF;
}

bool ObUnicodeBlockUtils::check_letter_connector(const ob_wc_t unicode)
{
  static constexpr char LETTER_CONNECTOR[] = {'#', '&', '+', '-', '.', '@', '_'};

  ob_wc_t code = unicode;
  if (code > 0xFF00 && code < 0xFF5F) {
    code -= 0xFEE0;
  }
  for (int i = 0; i < ARRAYSIZEOF(LETTER_CONNECTOR); i++) {
    if (code == LETTER_CONNECTOR[i]) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::check_num_connector(const ob_wc_t unicode)
{
  static constexpr char NUM_CONNECTOR[] = {',', '.'};
  ob_wc_t code = unicode;
  if (code > 0xFF00 && code < 0xFF5F) {
    code -= 0xFEE0;
  }
  for (int i = 0; i < ARRAYSIZEOF(NUM_CONNECTOR); i++) {
    if (code == NUM_CONNECTOR[i]) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::check_ignore_as_single(const ob_wc_t unicode)
{
  // cjk to ascii part
  if (unicode > 0xFF00 && unicode < 0xFF5F) {
    // if output as a single char, it will be ignored
    if (!is_alpha(unicode) && !is_arabic(unicode)) {
      return true;
    }
  }
  return false;
}

bool ObUnicodeBlockUtils::is_chinese(const ob_wc_t unicode)
{
  // variables name keep same with it's code block name, skip code style.
  bool CJK_UNIFIED_IDEOGRAPHS = (unicode >= 0x4E00 && unicode <= 0x9FFF);
  bool CJK_COMPATIBILITY_IDEOGRAPHS = (unicode >= 0xF900 && unicode <= 0xFAFF);
  bool CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A = (unicode >= 0x3400 && unicode <= 0x4DBF);
  return CJK_UNIFIED_IDEOGRAPHS || CJK_COMPATIBILITY_IDEOGRAPHS
         || CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A;
}

bool ObUnicodeBlockUtils::is_other_cjk(const ob_wc_t unicode)
{
  // variables name keep same with it's code block name, skip code style.
  bool HALFWIDTH_AND_FULLWIDTH_FORMS = (unicode >= 0xFF00 && unicode <= 0xFFEF);

  bool HANGUL_SYLLABLES = (unicode >= 0xAC00 && unicode <= 0xD7AF);
  bool HANGUL_JAMO = (unicode >= 0x1100 && unicode <= 0x11FF);
  bool HANGUL_COMPATIBILITY_JAMO = (unicode >= 0x3130 && unicode <= 0x318f);

  bool HIRAGANA = (unicode >= 0x3040 && unicode <= 0x309F);
  bool KATAKANA = (unicode >= 0x30A0 && unicode <= 0x30FF);
  bool KATAKANA_PHONETIC_EXTENSIONS = (unicode >= 0x31F0 && unicode <= 0x31FF);

  return HALFWIDTH_AND_FULLWIDTH_FORMS || HANGUL_SYLLABLES || HANGUL_JAMO
         || HANGUL_COMPATIBILITY_JAMO || HIRAGANA || KATAKANA || KATAKANA_PHONETIC_EXTENSIONS;
}

bool ObUnicodeBlockUtils::is_surrogate(const ob_wc_t unicode)
{
  bool HIGH_SURROGATES = (unicode >= 0xD800 && unicode <= 0xDB7F);
  bool LOW_SURROGATES = (unicode >= 0xDC00 && unicode <= 0xDFFF);

  return (HIGH_SURROGATES || LOW_SURROGATES);
}

bool ObUnicodeBlockUtils::is_convertable_fullwidth(const ob_wc_t unicode)
{
  bool FW_SPACE = (unicode == 0x3000);
  bool FW_OHTER = (unicode > 0xFF00 && unicode < 0xFF5F);
  return FW_SPACE || FW_OHTER;
}

namespace
{
// The characters recognized as Chinese numeric characters. The table is built
// once from the literals below and kept sorted and duplicate free, so that the
// per character check is a binary search instead of a linear scan.
class CnNumberSet
{
public:
  CnNumberSet() : size_(0)
  {
    static const char *CN_NUMBER_LITERALS[] = {
        "一", "二", "两", "三", "四", "五",
        "六", "七", "八", "九", "十", "零",
        "壹", "贰", "叁", "肆", "伍", "陆",
        "柒", "捌", "玖", "拾", "百", "千",
        "万", "亿", "拾", "佰", "仟", "萬",
        "億", "兆", "卅", "廿",
    };
    for (int64_t i = 0; i < ARRAYSIZEOF(CN_NUMBER_LITERALS); ++i) {
      const char *literal = CN_NUMBER_LITERALS[i];
      const int64_t literal_len = static_cast<int64_t>(STRLEN(literal));
      ob_wc_t unicode = 0;
      if (literal_len <= 0) {
      } else if (common::ob_charset_decode_unicode<common::CHARSET_UTF8MB4>(
                     reinterpret_cast<const unsigned char *>(literal),
                     reinterpret_cast<const unsigned char *>(literal + literal_len),
                     unicode)
                 <= 0) {
        // never happens, the literals above are valid utf8mb4
      } else {
        insert_sorted(unicode);
      }
    }
  }

  bool contains(const ob_wc_t unicode) const
  {
    int32_t left = 0;
    int32_t right = size_ - 1;
    bool found = false;
    while (!found && left <= right) {
      const int32_t mid = left + ((right - left) >> 1);
      if (values_[mid] == unicode) {
        found = true;
      } else if (values_[mid] < unicode) {
        left = mid + 1;
      } else {
        right = mid - 1;
      }
    }
    return found;
  }

private:
  void insert_sorted(const ob_wc_t unicode)
  {
    int32_t pos = 0;
    while (pos < size_ && values_[pos] < unicode) {
      ++pos;
    }
    if (pos < size_ && values_[pos] == unicode) {
      // already in the set
    } else if (size_ >= MAX_CN_NUMBER) {
      // never happens, CN_NUMBER_LITERALS is shorter than MAX_CN_NUMBER
    } else {
      for (int32_t i = size_; i > pos; --i) {
        values_[i] = values_[i - 1];
      }
      values_[pos] = unicode;
      ++size_;
    }
  }

private:
  static constexpr int32_t MAX_CN_NUMBER = 64;
  ob_wc_t values_[MAX_CN_NUMBER];
  int32_t size_;
};
} // namespace

bool ObUnicodeBlockUtils::is_unicode_cn_number(ob_wc_t unicode)
{
  // Built once; the guarded initialization below is thread safe.
  static const CnNumberSet CN_NUMBER_SET;
  return CN_NUMBER_SET.contains(unicode);
}

} //  namespace storage
} //  namespace oceanbase
