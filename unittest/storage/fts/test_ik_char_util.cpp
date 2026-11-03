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

#include <gtest/gtest.h>

#include "storage/fts/ik/ob_ik_char_util.h"

namespace oceanbase
{
namespace storage
{
namespace
{

using CharType = ObFTCharUtil::CharType;

class LogLevelGuard
{
public:
  LogLevelGuard() : previous_(OB_LOGGER.get_log_level())
  {
    OB_LOGGER.set_log_level("ERROR");
  }
  ~LogLevelGuard() { OB_LOGGER.set_log_level(previous_); }
private:
  int32_t previous_;
};

// Preserve the original UTF-8 classifier as an independent reference: each
// predicate decodes its input separately, in the original precedence order.
int classify_utf8_reference(const char *input, uint8_t length, CharType &type)
{
  type = CharType::USELESS;
  if (nullptr == input) {
    return OB_INVALID_ARGUMENT;
  }
  using Predicate = bool (*)(ob_wc_t);
  const Predicate predicates[] = {
      ObUnicodeBlockUtils::is_alpha,
      ObUnicodeBlockUtils::is_arabic,
      ObUnicodeBlockUtils::is_chinese,
      ObUnicodeBlockUtils::is_other_cjk};
  const CharType types[] = {
      CharType::ENGLISH_LETTER, CharType::ARABIC_LETTER,
      CharType::CHINESE, CharType::OTHER_CJK};
  const auto *begin = reinterpret_cast<const unsigned char *>(input);
  for (int i = 0; i < 4; ++i) {
    ob_wc_t codepoint = 0;
    if (common::ob_charset_decode_unicode<common::CHARSET_UTF8MB4>(
            begin, begin + length, codepoint) < 0) {
      return OB_UNEXPECT_INTERNAL_ERROR;
    } else if (predicates[i](codepoint)) {
      type = types[i];
      break;
    }
  }
  return OB_SUCCESS;
}

bool matches_reference(const char *input, uint8_t length)
{
  CharType expected = CharType::CHINESE;
  CharType actual = CharType::CHINESE;
  const int expected_ret = classify_utf8_reference(input, length, expected);
  const int actual_ret = ObFTCharUtil::classify_first_char(
      common::CS_TYPE_UTF8MB4_BIN, input, length, actual);
  if (expected_ret != actual_ret || expected != actual) {
    ADD_FAILURE() << "length=" << static_cast<int>(length)
                  << " expected_ret=" << expected_ret << " actual_ret=" << actual_ret
                  << " expected_type=" << static_cast<int>(expected)
                  << " actual_type=" << static_cast<int>(actual);
    return false;
  }
  return true;
}

} // namespace

TEST(IKCharClassification, preserves_all_utf8_codepoints_and_truncated_prefixes)
{
  LogLevelGuard log_level;
  unsigned char bytes[4];
  for (ob_wc_t cp = 0; cp <= 0x10ffff; ++cp) {
    const int length = common::ob_charset_encode_unicode<common::CHARSET_UTF8MB4>(
        cp, bytes, bytes + sizeof(bytes));
    ASSERT_GT(length, 0);
    for (int prefix = 0; prefix <= length; ++prefix) {
      if (!matches_reference(reinterpret_cast<const char *>(bytes), prefix)) {
        FAIL() << "codepoint=" << cp;
      }
    }
  }
}

TEST(IKCharClassification, preserves_every_one_and_two_byte_input)
{
  LogLevelGuard log_level;
  unsigned char bytes[2];
  ASSERT_TRUE(matches_reference(nullptr, 0));
  ASSERT_TRUE(matches_reference(nullptr, 4));
  for (unsigned first = 0; first < 256; ++first) {
    bytes[0] = first;
    ASSERT_TRUE(matches_reference(reinterpret_cast<const char *>(bytes), 1));
    for (unsigned second = 0; second < 256; ++second) {
      bytes[1] = second;
      if (!matches_reference(reinterpret_cast<const char *>(bytes), 2)) {
        FAIL() << "first=" << first << " second=" << second;
      }
    }
  }
}

} // namespace storage
} // namespace oceanbase
