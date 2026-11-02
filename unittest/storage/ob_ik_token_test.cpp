/*
 * Copyright (c) 2026 OceanBase.
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

#include <initializer_list>

#include <gtest/gtest.h>

#include "lib/allocator/page_arena.h"
#include "lib/ob_errno.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
namespace
{
ObIKToken make_token(const int64_t offset, const int64_t length, const int64_t char_count)
{
  ObIKToken token;
  token.ptr_ = nullptr;
  token.offset_ = offset;
  token.length_ = length;
  token.char_cnt_ = char_count;
  token.type_ = ObIKTokenType::IK_CHINESE_TOKEN;
  return token;
}

void expect_cache_matches_list(const ObIKTokenChain &chain)
{
  int64_t token_count = 0;
  int64_t x_weight = 1;
  int64_t p_weight = 0;
  int position = 0;
  for (const ObIKToken &token : chain.list().tokens()) {
    ++token_count;
    x_weight *= token.char_cnt_;
    ++position;
    p_weight += position * token.char_cnt_;
  }
  EXPECT_EQ(token_count, chain.token_count());
  EXPECT_EQ(x_weight, chain.x_weight());
  EXPECT_EQ(p_weight, chain.p_weight());
}

int64_t original_x_weight(const ObIKTokenChain &chain)
{
  int64_t weight = 1;
  for (const ObIKToken &token : chain.list().tokens()) {
    weight *= token.char_cnt_;
  }
  return weight;
}

int64_t original_p_weight(const ObIKTokenChain &chain)
{
  int64_t weight = 0;
  int position = 0;
  for (const ObIKToken &token : chain.list().tokens()) {
    ++position;
    weight += position * token.char_cnt_;
  }
  return weight;
}

bool original_better_than(const ObIKTokenChain &left, const ObIKTokenChain &right)
{
  if (&left == &right) {
    return false;
  }
  if (left.payload() != right.payload()) {
    return left.payload() > right.payload();
  }
  const int64_t left_count = left.list().tokens().size();
  const int64_t right_count = right.list().tokens().size();
  if (left_count != right_count) {
    return left_count < right_count;
  }
  if (left.offset_len() != right.offset_len()) {
    return left.offset_len() > right.offset_len();
  }
  if (left.max_offset() != right.max_offset()) {
    return left.max_offset() > right.max_offset();
  }
  const int64_t left_x_weight = original_x_weight(left);
  const int64_t right_x_weight = original_x_weight(right);
  if (left_x_weight != right_x_weight) {
    return left_x_weight > right_x_weight;
  }
  return original_p_weight(left) > original_p_weight(right);
}

void add_tokens(ObIKTokenChain &chain, std::initializer_list<ObIKToken> tokens)
{
  for (const ObIKToken &token : tokens) {
    bool added = false;
    ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(token, added));
    ASSERT_TRUE(added);
    expect_cache_matches_list(chain);
  }
}

void expect_comparison_matches_original(common::ObArenaAllocator &allocator,
                                        std::initializer_list<ObIKToken> left_tokens,
                                        std::initializer_list<ObIKToken> right_tokens)
{
  ObIKTokenChain left(allocator);
  ObIKTokenChain right(allocator);
  add_tokens(left, left_tokens);
  add_tokens(right, right_tokens);
  EXPECT_EQ(original_better_than(left, right), left.better_than(right));
  EXPECT_EQ(original_better_than(right, left), right.better_than(left));
  EXPECT_FALSE(left.better_than(left));
}
} // namespace

TEST(ObIKTokenChainTest, cache_tracks_insertions_duplicates_and_pops)
{
  common::ObArenaAllocator allocator(lib::ObMemAttr("IKTokenTest"));
  ObIKTokenChain chain(allocator);
  bool added = false;

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(20, 2, 3), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(30, 2, 2), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(0, 2, 4), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_conflict(make_token(10, 2, 5), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);
  ASSERT_EQ(4, chain.token_count());

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_conflict(make_token(10, 2, 5), added));
  ASSERT_TRUE(added);
  EXPECT_EQ(4, chain.token_count());
  expect_cache_matches_list(chain);

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(10, 2, 5), added));
  EXPECT_FALSE(added);
  EXPECT_EQ(4, chain.token_count());
  expect_cache_matches_list(chain);

  ObIKToken popped;
  while (chain.token_count() > 0) {
    ASSERT_EQ(OB_SUCCESS, chain.pop_back(popped));
    expect_cache_matches_list(chain);
  }
  EXPECT_EQ(0, chain.token_count());
  EXPECT_EQ(1, chain.x_weight());
  EXPECT_EQ(0, chain.p_weight());

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(40, 1, 7), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);
  EXPECT_EQ(7, chain.p_weight());

  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(50, 1, 2), added));
  ASSERT_TRUE(added);
  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(60, 1, 3), added));
  ASSERT_TRUE(added);
  ASSERT_EQ(OB_SUCCESS, chain.pop_back(popped));
  ASSERT_EQ(OB_SUCCESS, chain.pop_back(popped));
  EXPECT_EQ(1, chain.token_count());
  ASSERT_EQ(OB_SUCCESS, chain.add_token_if_no_conflict(make_token(70, 1, 4), added));
  ASSERT_TRUE(added);
  expect_cache_matches_list(chain);
  ASSERT_EQ(OB_SUCCESS, chain.pop_back(popped));
  expect_cache_matches_list(chain);
  ASSERT_EQ(OB_SUCCESS, chain.pop_back(popped));
  expect_cache_matches_list(chain);
  EXPECT_EQ(0, chain.token_count());
}

TEST(ObIKTokenChainTest, copy_and_self_copy_preserve_chain_and_weights)
{
  common::ObArenaAllocator allocator(lib::ObMemAttr("IKTokenCopyTest"));
  ObIKTokenChain source(allocator);
  ObIKTokenChain copy(allocator);
  add_tokens(source, {make_token(0, 2, 2), make_token(4, 3, 5), make_token(10, 1, 3)});

  EXPECT_EQ(OB_INVALID_ARGUMENT, copy.copy(nullptr));
  expect_cache_matches_list(copy);

  ASSERT_EQ(OB_SUCCESS, copy.copy(&source));
  expect_cache_matches_list(copy);
  EXPECT_EQ(source.token_count(), copy.token_count());
  EXPECT_EQ(source.x_weight(), copy.x_weight());
  EXPECT_EQ(source.p_weight(), copy.p_weight());
  EXPECT_FALSE(source.better_than(copy));
  EXPECT_FALSE(copy.better_than(source));

  ASSERT_EQ(OB_SUCCESS, source.copy(&source));
  expect_cache_matches_list(source);
  EXPECT_EQ(3, source.token_count());
}

TEST(ObIKTokenChainTest, better_than_matches_original_comparison_order)
{
  common::ObArenaAllocator allocator(lib::ObMemAttr("IKTokenCompareTest"));
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 2, 1)},
                                    {make_token(0, 1, 1)});
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 2, 1), make_token(2, 2, 1)},
                                    {make_token(0, 4, 1)});
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 1, 1), make_token(4, 1, 1)},
                                    {make_token(0, 1, 1), make_token(2, 1, 1)});
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 1, 1), make_token(3, 1, 1)},
                                    {make_token(10, 1, 1), make_token(13, 1, 1)});
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 1, 1), make_token(3, 1, 3)},
                                    {make_token(0, 1, 2), make_token(3, 1, 2)});
  expect_comparison_matches_original(allocator,
                                    {make_token(0, 1, 1), make_token(3, 1, 4)},
                                    {make_token(0, 1, 2), make_token(3, 1, 2)});
}
} // namespace storage
} // namespace oceanbase

int main(int argc, char **argv)
{
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
