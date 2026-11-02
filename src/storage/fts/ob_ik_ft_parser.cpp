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

#include "src/storage/fts/ob_ik_ft_parser.h"

#include "lib/charset/ob_charset.h"
#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "lib/utility/utility.h"
#include "data_plane/fts/ob_fts_struct.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
#include "storage/fts/dict/ob_ft_dict_hub.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/ik/ob_ik_arbitrator.h"
#include "storage/fts/ik/ob_ik_cjk_processor.h"
#include "storage/fts/ik/ob_ik_letter_processor.h"
#include "storage/fts/ik/ob_ik_processor.h"
#include "storage/fts/ik/ob_ik_quantifier_processor.h"
#include "storage/fts/ik/ob_ik_surrogate_processor.h"

namespace oceanbase
{
namespace storage
{
int ObIKFTParser::init(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    coll_type_ = ObCollationType::CS_TYPE_INVALID;
    if (OB_ISNULL(param.cs_) || OB_ISNULL(param.cs_->name)) {
      ret = OB_INVALID_ARGUMENT;
    } else if (CS_TYPE_INVALID == (coll_type_ = ObCharset::collation_type(param.cs_->name))) {
      ret = OB_INVALID_ARGUMENT;
    } else if (OB_FAIL(init_dict(param))) {
    } else if (OB_FAIL(init_ctx(param))) {
    } else if (OB_FAIL(init_segmenter(param))) {
    }

    if (OB_FAIL(ret)) {
      reset();
    } else {
      is_inited_ = true;
    }
  }

  return ret;
}

int ObIKFTParser::reuse(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(param.cs_) || OB_ISNULL(param.cs_->name)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (ObCharset::collation_type(param.cs_->name) != coll_type_) {
    // different collation, the dictionaries were built for another charset
    ret = OB_INVALID_ARGUMENT;
  } else if ((ObFTIKParam::Mode::SMART == param.ik_param_.mode_) != ctx_->is_smart()) {
    // the smart mode is fixed when the context is created
    ret = OB_INVALID_ARGUMENT;
  } else {
    // The dictionaries only depend on the table contents, which do not change
    // for the lifetime of the process, so they are kept.  Everything that
    // carries per document state is rebuilt from scratch.
    reset_document_state();
    if (OB_FAIL(init_ctx(param))) {
    } else if (OB_FAIL(init_segmenter(param))) {
    }
  }
  if (OB_FAIL(ret)) {
    reset();
  }
  return ret;
}

int ObIKFTParser::get_next_token(const char *&word,
                                 int64_t &word_len,
                                 int64_t &char_cnt,
                                 int64_t &word_freq)
{
  int ret = OB_SUCCESS;
  const char *output_word;
  int64_t len;
  int64_t offset;
  int64_t cnt;

  if (!IS_INIT) {
    ret = OB_NOT_INIT;
  } else {
    bool accept_token = false;
    while (OB_SUCC(ret) && !accept_token) {
      if (OB_FAIL(produce())) {
      } else if (OB_FAIL(ctx_->get_next_token(output_word, len, offset, cnt))) {
        if (OB_ITER_END == ret) {
          // ok, end this iter
        } else {
        }
      } else {
        bool is_stop = false;
        // if (!OB_ISNULL(dict_stop_)
        //     && OB_FAIL(dict_stop_->match(ObString(len, output_word + offset), is_stop))) {
        //   LOG_WARN("Failed to match stopwords", K(ret));
        // } else
        if (!is_stop) {
          word = output_word + offset;
          word_len = len;
          char_cnt = cnt;
          word_freq = 1;
          accept_token = true;
        } else {
        }
      }
    }
  }

  return ret;
}

int ObIKFTParser::produce()
{
  int ret = OB_SUCCESS;
  // Loop until end or has data to output
  while (OB_SUCC(ret) && ctx_->result_list().empty() && !ctx_->iter_end()) {
    if (OB_FAIL(process_next_batch())) {
      if (OB_ITER_END == ret) {
        // ok
      } else {
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
  }
  return ret;
}

int ObIKFTParser::process_one_char(TokenizeContext &ctx,
                                   const char *ch,
                                   const uint8_t char_len,
                                   const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;
  // The processors are called in their fixed order, but only when the
  // character can actually affect them.  A processor that has no open state
  // and does not handle this character class is a guaranteed no-op, so it is
  // skipped; every skipped call keeps the token stream identical.
  const bool is_letter = (ObFTCharUtil::CharType::ENGLISH_LETTER == type
                          || ObFTCharUtil::CharType::ARABIC_LETTER == type);
  const bool is_chinese = (ObFTCharUtil::CharType::CHINESE == type);
  const bool is_surrogate = (ObFTCharUtil::CharType::SURROGATE_HIGH == type
                             || ObFTCharUtil::CharType::SURROGATE_LOW == type);
  if (is_letter || letter_seg_->has_open_state()) {
    ret = letter_seg_->do_process(ctx, ch, char_len, type);
  }
  if (OB_SUCC(ret) && (is_chinese || quantifier_seg_->has_open_state())) {
    ret = quantifier_seg_->do_process(ctx, ch, char_len, type);
  }
  if (OB_SUCC(ret)
      && (ObFTCharUtil::CharType::USELESS != type || cjk_seg_->has_pending_hits())) {
    ret = cjk_seg_->do_process(ctx, ch, char_len, type);
  }
  if (OB_SUCC(ret) && (is_surrogate || surrogate_seg_->has_open_state())) {
    ret = surrogate_seg_->do_process(ctx, ch, char_len, type);
  }
  return ret;
}

int ObIKFTParser::process_next_batch()
{
  int ret = OB_SUCCESS;
  ctx_->reset_resource();

  // handle next segmenter
  bool do_seg = false;

  if (ctx_->iter_end()) {
    ret = OB_ITER_END;
  } else {
    while (OB_SUCC(ret) && !do_seg && !ctx_->iter_end()) {
      const char *ch;
      uint8_t char_len = 0;
      ObFTCharUtil::CharType type = ObFTCharUtil::CharType::USELESS;
      if (OB_FAIL(ctx_->current_char(ch, char_len))) {
      } else if (OB_FAIL(ctx_->current_char_type(type))) {
      } else if (OB_FAIL(process_one_char(*ctx_, ch, char_len, type))) {
      } else {
        // 1. check segmention
        if (ctx_->handle_size() > SEGMENT_LIMIT && type == ObFTCharUtil::CharType::USELESS) {
          do_seg = true;
        }

        // 2. move to next;
        if (OB_FAIL(ctx_->step_next())) {
          if (OB_ITER_END == ret) {
          } else {
          }
        }
      } // end of one batch
    }

    if (OB_SUCC(ret) || OB_ITER_END == ret) {
      ObIKArbitrator arb;
      if (OB_FAIL(arb.process(*ctx_))) {
      } else if (OB_FAIL(arb.output_result(*ctx_))) {
      }
    } else {
      // Already logged.
    }
  }

  return ret;
}

namespace
{
// Building the dictionaries of the IK parser costs a few microseconds and they
// only depend on table contents that do not change while the process runs, yet a
// fresh parser is created for every document.  The parser of a thread is
// therefore kept and re-initialized for the next document (see
// ObIKFTParser::reuse).  Its memory comes from a dedicated arena: reusing the
// parser means that the per document allocations (tokenize context and the token
// lists) cannot be handed back to the caller's allocator because an arena
// ignores free(), so the whole cache is dropped and rebuilt once the arena has
// grown past the water mark below.  One document leaves a few kilobytes behind,
// so the dictionary rebuild (a few microseconds) is amortized over ~100
// documents while every tokenizing thread stays below this bound.
constexpr int64_t IK_PARSER_CACHE_MAX_BYTES = 512 * 1024;

struct ObIKParserCache
{
  ObArenaAllocator alloc_;
  ObIKFTParser *parser_;
  ObFTDictHub *hub_;
  bool in_use_;

  ObIKParserCache() : alloc_(ObMemAttr("IKParserCache")), parser_(nullptr), hub_(nullptr), in_use_(false) {}
  ~ObIKParserCache() { destroy(); }

  void destroy()
  {
    if (nullptr != parser_) {
      parser_->~ObIKFTParser();
      alloc_.free(parser_);
      parser_ = nullptr;
    }
    alloc_.reset();
    hub_ = nullptr;
    in_use_ = false;
  }

  bool is_reusable(ObFTDictHub *hub) const
  {
    return nullptr != parser_ && !in_use_ && hub == hub_
           && alloc_.used() < IK_PARSER_CACHE_MAX_BYTES;
  }
};

ObIKParserCache &get_ik_parser_cache()
{
  static thread_local ObIKParserCache cache;
  return cache;
}
} // namespace

int ObIKFTParserDesc::segment(ObFTParserParam *param, ObITokenIterator *&iter) const
{
  int ret = OB_SUCCESS;
  ObIKFTParser *parser = nullptr;
  ObFTDictHub *hub = nullptr;
  ObIKParserCache &cache = get_ik_parser_cache();
  if (OB_ISNULL(param) || OB_UNLIKELY(!param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObFTParseData::instance().get_dict_hub(hub))) {
  } else {
    bool reused = false;
    if (cache.is_reusable(hub)) {
      const int reuse_ret = cache.parser_->reuse(*param);
      if (OB_SUCCESS == reuse_ret) {
        parser = cache.parser_;
        reused = true;
      } else {
        // Either the configuration changed or the parser cannot take another
        // document (e.g. it failed to rebuild its per document state).  Drop it
        // and build a fresh one, exactly like a cold call, so that a single
        // failure never poisons the cache of this thread.
        cache.destroy();
      }
    } else if (nullptr != cache.parser_ && !cache.in_use_) {
      // The arena grew too large, drop it and start over.
      cache.destroy();
    } else {
      // A parser of this thread is currently in use (nested evaluation), fall
      // back to a one shot parser.
    }

    if (OB_FAIL(ret)) {
    } else if (!reused) {
      if (OB_ISNULL(cache.parser_) && !cache.in_use_) {
        if (OB_ISNULL(parser = OB_NEWx(ObIKFTParser, &cache.alloc_, cache.alloc_, hub))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(parser->init(*param))) {
          OB_DELETEx(ObIKFTParser, &cache.alloc_, parser);
          parser = nullptr;
        } else {
          cache.parser_ = parser;
          cache.hub_ = hub;
        }
      } else if (OB_ISNULL(
                     parser = OB_NEWx(ObIKFTParser, param->allocator_, *(param->allocator_), hub))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(parser->init(*param))) {
        OB_DELETEx(ObIKFTParser, param->allocator_, parser);
        parser = nullptr;
      }
    }

    if (OB_SUCC(ret)) {
      if (OB_ISNULL(parser)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        if (parser == cache.parser_) {
          cache.in_use_ = true;
        }
        iter = parser;
      }
    }
  }
  return ret;
}

void ObIKFTParserDesc::free_token_iter(ObFTParserParam *param,
                                       ObITokenIterator *&iter) const
{
  ObIKParserCache &cache = get_ik_parser_cache();
  if (OB_ISNULL(iter)) {
  } else if (iter == cache.parser_) {
    // Keep the parser for the next document of this thread.
    cache.in_use_ = false;
    iter = nullptr;
  } else {
    iter->~ObITokenIterator();
    if (OB_NOT_NULL(param) && OB_NOT_NULL(param->allocator_)) {
      param->allocator_->free(iter);
    }
    iter = nullptr;
  }
}

int ObIKFTParserDesc::get_add_word_flag(ObAddWordFlag &flag) const
{
  int ret = OB_SUCCESS;
  flag.set_casedown();
  flag.set_groupby_word();
  return ret;
}

int ObIKFTParser::init_dict(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  ObIFTDict *tmp_dict = nullptr;

  if (OB_ISNULL(hub_)) {
    ret = OB_ERR_UNEXPECTED;
  }

  ObFTRangeDict *dict = nullptr;
  ObFTDictDesc main_dict_desc("main_dict",
                              ObFTDictType::DICT_IK_MAIN,
                              ObCharsetType::CHARSET_UTF8MB4,
                              ObCollationType::CS_TYPE_UTF8MB4_BIN);

  ObFTDictDesc quan_dict_desc("quan_dict",
                              ObFTDictType::DICT_IK_QUAN,
                              ObCharsetType::CHARSET_UTF8MB4,
                              ObCollationType::CS_TYPE_UTF8MB4_BIN);

  ObFTDictDesc stopword_dict_desc("stopword",
                                  ObFTDictType::DICT_IK_STOP,
                                  ObCharsetType::CHARSET_UTF8MB4,
                                  ObCollationType::CS_TYPE_UTF8MB4_BIN);

  if (should_read_newest_table()) {
    // clear dict cache, always false now
  } else {
    if (OB_FAIL(init_single_dict(main_dict_desc, cache_main_))) {
    } else if (OB_FAIL(init_single_dict(quan_dict_desc, cache_quan_))) {
    } else if (OB_FAIL(init_single_dict(stopword_dict_desc, cache_stop_))) {
    }
  }

  if (OB_FAIL(ret)) {
    // already logged.
  } else if (OB_FAIL(build_dict_from_cache(main_dict_desc, cache_main_, dict_main_))) {
  } else if (OB_FAIL(build_dict_from_cache(quan_dict_desc, cache_quan_, dict_quan_))) {
  } else if (OB_FAIL(build_dict_from_cache(stopword_dict_desc, cache_stop_, dict_stop_))) {
  }

  return ret;
}

int ObIKFTParser::init_single_dict(ObFTDictDesc desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(hub_->load_cache(desc, container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      if (OB_FAIL(hub_->build_cache(desc, container))) {
      }
    } else {
    }
  }
  return ret;
}

int ObIKFTParser::init_ctx(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;

  if (coll_type_ == common::CS_TYPE_INVALID) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(ctx_ = OB_NEWx(TokenizeContext,
                                      &allocator_,
                                      coll_type_,
                                      allocator_,
                                      param.fulltext_,
                                      param.ft_length_,
                                      param.ik_param_.mode_ == ObFTIKParam::Mode::SMART))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ctx_->init())) {
  }
  if (OB_FAIL(ret)) {
    OB_DELETEx(TokenizeContext, &allocator_, ctx_);
  }
  return ret;
}

int ObIKFTParser::init_segmenter(const ObFTParserParam &param)
{
  int ret = OB_SUCCESS;
  // do have an order
  ObIKLetterProcessor *letter_seg = nullptr;
  ObIKQuantifierProcessor *cnqsg = nullptr;
  ObIKCJKProcessor *cjksg = nullptr;
  ObIKSurrogateProcessor *surrogate_seg = nullptr;
  if (OB_ISNULL(letter_seg = OB_NEWx(ObIKLetterProcessor, &allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(dict_quan_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(cnqsg = OB_NEWx(ObIKQuantifierProcessor, &allocator_, *dict_quan_, allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(dict_main_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(cjksg = OB_NEWx(ObIKCJKProcessor, &allocator_, *dict_main_, allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(surrogate_seg = OB_NEWx(ObIKSurrogateProcessor, &allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(segmenters_.push_back(letter_seg))) {
  } else if (FALSE_IT(letter_seg_ = letter_seg)) {
  } else if (FALSE_IT(letter_seg = nullptr)) {
  } else if (OB_FAIL(segmenters_.push_back(cnqsg))) {
  } else if (FALSE_IT(quantifier_seg_ = cnqsg)) {
  } else if (FALSE_IT(cnqsg = nullptr)) {
  } else if (OB_FAIL(segmenters_.push_back(cjksg))) {
  } else if (FALSE_IT(cjk_seg_ = cjksg)) {
  } else if (FALSE_IT(cjksg = nullptr)) {
  } else if (OB_FAIL(segmenters_.push_back(surrogate_seg))) {
  } else if (FALSE_IT(surrogate_seg_ = surrogate_seg)) {
  } else if (OB_FALSE_IT(surrogate_seg = nullptr)) {
  }
  // push back by order, quantifier is before cjk

  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIKLetterProcessor, &allocator_, letter_seg);
    OB_DELETEx(ObIKQuantifierProcessor, &allocator_, cnqsg);
    OB_DELETEx(ObIKCJKProcessor, &allocator_, cjksg);
    OB_DELETEx(ObIKSurrogateProcessor, &allocator_, surrogate_seg);
  }
  return ret;
}

void ObIKFTParser::reset_document_state()
{
  if (!OB_ISNULL(ctx_)) {
    ctx_->~TokenizeContext();
    allocator_.free(ctx_);
    ctx_ = nullptr;
  }

  for (ObIIKProcessor *segmenter : segmenters_) {
    if (!OB_ISNULL(segmenter)) {
      segmenter->~ObIIKProcessor();
      allocator_.free(segmenter);
    }
  }
  segmenters_.clear();
  letter_seg_ = nullptr;
  quantifier_seg_ = nullptr;
  cjk_seg_ = nullptr;
  surrogate_seg_ = nullptr;
}

void ObIKFTParser::destroy_dicts()
{
  cache_main_.reset();
  cache_quan_.reset();
  cache_stop_.reset();

  if (!OB_ISNULL(dict_main_)) {
    dict_main_->~ObIFTDict();
    allocator_.free(dict_main_);
    dict_main_ = nullptr;
  }
  if (!OB_ISNULL(dict_quan_)) {
    dict_quan_->~ObIFTDict();
    allocator_.free(dict_quan_);
    dict_quan_ = nullptr;
  }
  if (!OB_ISNULL(dict_stop_)) {
    dict_stop_->~ObIFTDict();
    allocator_.free(dict_stop_);
    dict_stop_ = nullptr;
  }
}

void ObIKFTParser::reset()
{
  reset_document_state();
  destroy_dicts();
  is_inited_ = false;
}

bool ObIKFTParser::should_read_newest_table() const { return false; }
int ObIKFTParser::build_dict_from_cache(const ObFTDictDesc &desc,
                                        ObFTCacheRangeContainer &container,
                                        ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(hub_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(dict = OB_NEWx(ObFTRangeDict, &allocator_, allocator_, &container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(dict->init())) {
  }
  if (OB_FAIL(ret)) {
    OB_DELETEx(ObIFTDict, &allocator_, dict);
  }
  return ret;
}

} //  namespace storage
} //  namespace oceanbase
