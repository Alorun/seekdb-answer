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

#ifndef OB_BENG_FT_PARSER_H_
#define OB_BENG_FT_PARSER_H_

#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/ob_print_utils.h"
#include "share/text_analysis/ob_text_analyzer.h"
#include "data_plane/fts/ob_fts_parser.h"

namespace oceanbase
{
namespace storage
{

class ObBEngFTParser final : public ObITokenIterator
{
public:
  static const int64_t FT_MIN_WORD_LEN = 3;
  static const int64_t FT_MAX_WORD_LEN = 84;
public:
  explicit ObBEngFTParser(common::ObIAllocator &allocator)
    : allocator_(allocator),
      analysis_ctx_(),
      english_analyzer_(),
      doc_(),
      token_stream_(nullptr),
      is_inited_(false),
      fast_ascii_(false),
      out_buf_(nullptr),
      scan_pos_(0)
  {}
  ~ObBEngFTParser() { reset(); }

  int init(ObFTParserParam *param);
  void reset();
  virtual int get_next_token(
      const char *&word,
      int64_t &word_len,
      int64_t &char_len,
      int64_t &word_freq) override;

  VIRTUAL_TO_STRING_KV(K_(analysis_ctx), K_(english_analyzer), KP_(token_stream), K_(is_inited));
private:
  int segment(
      const common::ObDatum &doc,
      share::ObITokenStream *&token_stream);

  // ASCII fast path, used when the whole document is ASCII.  For ASCII the
  // charset classification is the C one: every byte below 0x80 is either
  // [0-9A-Za-z] or one of the whitespace/control/punctuation bytes the
  // whitespace tokenizer splits on, and the basic English normalizer only
  // lowercases it.  The path therefore produces the very same token sequence
  // (same bytes, same order, same frequency) with a single scan and one table
  // lookup per byte, without the analyzer pipeline.
  static bool fast_ascii_enabled();
  static bool is_pure_ascii(const char *ptr, const int64_t len);
  int get_next_token_ascii(const char *&word,
                           int64_t &word_len,
                           int64_t &char_len,
                           int64_t &word_freq);
private:
  common::ObIAllocator &allocator_;
  share::ObTextAnalysisCtx analysis_ctx_;
  share::ObEnglishTextAnalyzer english_analyzer_;
  common::ObDatum doc_;
  share::ObITokenStream *token_stream_;
  bool is_inited_;
  // ASCII fast path state: the lowercased token copies are owned by the
  // caller's allocator, which is the same lifetime the normalizer used.
  bool fast_ascii_;
  char *out_buf_;
  int64_t scan_pos_;

  DISALLOW_COPY_AND_ASSIGN(ObBEngFTParser);
};

class ObBasicEnglishFTParserDesc final : public ObIFTParserDesc
{
public:
  ObBasicEnglishFTParserDesc();
  virtual ~ObBasicEnglishFTParserDesc() = default;
  virtual int segment(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual void free_token_iter(ObFTParserParam *param, ObITokenIterator *&iter) const override;
  virtual int get_add_word_flag(ObAddWordFlag &flag) const override;
};

} // end namespace storage
} // end namespace oceanbase

#endif // OB_BENG_FT_PARSER_H_
