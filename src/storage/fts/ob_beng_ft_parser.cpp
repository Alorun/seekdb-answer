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

#include <cstdlib>

#include "storage/fts/ob_beng_ft_parser.h"
#include "data_plane/fts/ob_fts_struct.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace storage
{

namespace
{
// Membership in [0-9A-Za-z] for the bytes that can appear in an ASCII document.
// This is exactly what ob_isalnum() reports for them (ctype flag _MY_U/_MY_L/
// _MY_NMR in ctype_utf8mb4), and the bytes that are not in it are exactly the
// ones ob_isspace() || ob_iscntrl() || ob_ispunct() splits on, so the table also
// describes the tokens of the whitespace tokenizer.
struct AsciiAlnumTable
{
  bool value_[256];
  constexpr AsciiAlnumTable() : value_()
  {
    for (int c = '0'; c <= '9'; ++c) {
      value_[c] = true;
    }
    for (int c = 'A'; c <= 'Z'; ++c) {
      value_[c] = true;
    }
    for (int c = 'a'; c <= 'z'; ++c) {
      value_[c] = true;
    }
  }
};
constexpr AsciiAlnumTable ASCII_ALNUM;

OB_INLINE bool is_ascii_alnum(const char c)
{
  return ASCII_ALNUM.value_[static_cast<uint8_t>(c)];
}
} // namespace

bool ObBEngFTParser::fast_ascii_enabled()
{
  static const bool enabled = []() -> bool {
    const char *value = ::getenv("OB_FTS_BENG_FAST");
    return !(nullptr != value && '0' == value[0]);
  }();
  return enabled;
}

bool ObBEngFTParser::is_pure_ascii(const char *ptr, const int64_t len)
{
  bool is_ascii = (nullptr != ptr && len > 0);
  for (int64_t i = 0; is_ascii && i < len; ++i) {
    is_ascii = (0 == (static_cast<uint8_t>(ptr[i]) & 0x80));
  }
  return is_ascii;
}

int ObBEngFTParser::get_next_token_ascii(const char *&word,
                                         int64_t &word_len,
                                         int64_t &char_len,
                                         int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  const char *doc = doc_.ptr_;
  const int64_t len = doc_.len_;
  int64_t pos = scan_pos_;
  while (pos < len && !is_ascii_alnum(doc[pos])) {
    ++pos;
  }
  if (OB_UNLIKELY(pos >= len)) {
    scan_pos_ = len;
    ret = OB_ITER_END;
  } else {
    const int64_t start = pos;
    while (pos < len && is_ascii_alnum(doc[pos])) {
      ++pos;
    }
    const int64_t token_len = pos - start;
    // Lowercase into the copy owned by the caller's allocator, which is where
    // the basic English normalizer used to place its output.
    char *dst = out_buf_ + start;
    for (int64_t i = 0; i < token_len; ++i) {
      const uint8_t c = static_cast<uint8_t>(doc[start + i]);
      dst[i] = static_cast<char>((c >= 'A' && c <= 'Z') ? (c + ('a' - 'A')) : c);
    }
    scan_pos_ = pos;
    word = dst;
    word_len = token_len;
    char_len = token_len;
    word_freq = 1;
  }
  return ret;
}

int ObBEngFTParser::get_next_token(
    const char *&word,
    int64_t &word_len,
    int64_t &char_len,
    int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  ObDatum token;
  int64_t token_freq = 0;
  word = nullptr;
  word_len = 0;
  char_len = 0;
  word_freq = 0;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_UNLIKELY(fast_ascii_)) {
    ret = get_next_token_ascii(word, word_len, char_len, word_freq);
  } else if (OB_ISNULL(token_stream_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(token_stream_->get_next(token, token_freq))) {
    if (OB_ITER_END != ret) {
    }
  } else if (OB_ISNULL(token.ptr_) || OB_UNLIKELY(0 >= token.len_ || 0 >= token_freq)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    word = token.ptr_;
    word_len = token.len_;
    char_len = token.len_;
    word_freq = token_freq;
    LOG_DEBUG("succeed to add word", K(ObString(word_len, word)), K(word_freq));
  }
  return ret;
}

int ObBEngFTParser::init(ObFTParserParam *param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < param->ft_length_)) {
    ret = OB_NOT_SUPPORTED;
  } else {
    doc_.set_string(param->fulltext_, param->ft_length_);
    analysis_ctx_.cs_ = param->cs_;
    analysis_ctx_.filter_stopword_ = false;
    analysis_ctx_.need_grouping_ = false;
    if (OB_UNLIKELY(fast_ascii_enabled())
        && is_pure_ascii(param->fulltext_, param->ft_length_)) {
      // The whole document is ASCII: the analyzer pipeline is not needed, the
      // tokens are the runs of [0-9A-Za-z] lowercased in place (see
      // get_next_token_ascii) and they are produced in document order.
      if (OB_ISNULL(out_buf_ = static_cast<char *>(param->allocator_->alloc(param->ft_length_)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        fast_ascii_ = true;
        scan_pos_ = 0;
        is_inited_ = true;
      }
    } else if (OB_FAIL(english_analyzer_.init(analysis_ctx_, *param->allocator_))) {
    } else if (OB_FAIL(segment(doc_, token_stream_))) {
    } else if (OB_ISNULL(token_stream_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      is_inited_ = true;
    }
  }
  if (OB_FAIL(ret) && OB_UNLIKELY(!is_inited_)) {
    reset();
  }
  return ret;
}

int ObBEngFTParser::segment(
    const common::ObDatum &doc,
    share::ObITokenStream *&token_stream)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(doc.ptr_) || OB_UNLIKELY(0 >= doc.len_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(UINT32_MAX < doc.len_)) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(english_analyzer_.analyze(doc, token_stream))) {
  }
  return ret;
}

void ObBEngFTParser::reset()
{
  analysis_ctx_.reset();
  english_analyzer_.reset();
  doc_.reset();
  token_stream_ = nullptr;
  is_inited_ = false;
  fast_ascii_ = false;
  out_buf_ = nullptr;
  scan_pos_ = 0;
}

ObBasicEnglishFTParserDesc::ObBasicEnglishFTParserDesc() {}

int ObBasicEnglishFTParserDesc::segment(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObBEngFTParser *parser = nullptr;
  if (OB_ISNULL(param) || OB_ISNULL(param->fulltext_) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(parser = OB_NEWx(ObBEngFTParser, param->allocator_, *(param->allocator_)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(parser->init(param))) {
  } else {
    iter = parser;
  }

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObBEngFTParser, param->allocator_, parser);
  }

  return ret;
}

void ObBasicEnglishFTParserDesc::free_token_iter(
    ObFTParserParam *param,
    ObITokenIterator *&iter) const
{
  if (OB_NOT_NULL(iter)) {
    abort_unless(nullptr != param);
    abort_unless(nullptr != param->allocator_);
    iter->~ObITokenIterator();
    param->allocator_->free(iter);
  }
}

int ObBasicEnglishFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_min_max_word();
  flag.set_stop_word();
  flag.set_groupby_word();
  return ret;
}

} // end namespace storage
} // end namespace oceanbase
