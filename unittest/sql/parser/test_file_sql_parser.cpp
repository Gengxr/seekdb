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

#include "sql/parser/ob_parser.h"
#include "lib/allocator/page_arena.h"
#include "common/sql_mode/ob_sql_mode.h"
#include <gtest/gtest.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

class TestFileSqlParser : public ::testing::Test
{
protected:
  int parse(const char *sql, const ParseNode *&stmt)
  {
    int ret = OB_SUCCESS;
    ObParser parser(allocator_, DEFAULT_MYSQL_MODE);
    stmt = nullptr;
    if (OB_SUCC(parser.parse(ObString::make_string(sql), result_))) {
      if (OB_ISNULL(result_.result_tree_) || result_.result_tree_->num_child_ < 1) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        stmt = result_.result_tree_->children_[0];
      }
    }
    return ret;
  }
  static const ParseNode *find(const ParseNode *node, const ObItemType type)
  {
    const ParseNode *found = nullptr;
    if (OB_NOT_NULL(node)) {
      if (type == node->type_) {
        found = node;
      }
      for (int64_t i = 0; OB_ISNULL(found) && i < node->num_child_; ++i) {
        found = find(node->children_[i], type);
      }
    }
    return found;
  }
  ObArenaAllocator allocator_;
  ParseResult result_;
};

TEST_F(TestFileSqlParser, read_functions_and_alias)
{
  struct Case
  {
    const char *sql_;
    int64_t func_;
  } cases[] = {
    {"select * from read_csv('/d/a.csv')", 1},
    {"select * from read_json('/d/a.jsonl') j", 2},
    {"select * from read_json_auto('/d/a.jsonl')", 2},
    {"select * from read_ndjson('/d/a.jsonl') as j", 3},
    {"select c1 from read_parquet('/d/a.parquet') p", 4},
    {"select file from glob('/d/*')", 5},
  };
  for (int64_t i = 0; i < static_cast<int64_t>(sizeof(cases) / sizeof(cases[0])); ++i) {
    const ParseNode *stmt = nullptr;
    ASSERT_EQ(OB_SUCCESS, parse(cases[i].sql_, stmt)) << cases[i].sql_;
    const ParseNode *table = find(stmt, T_FILE_TABLE);
    ASSERT_NE(nullptr, table) << cases[i].sql_;
    ASSERT_EQ(4, table->num_child_);
    EXPECT_EQ(cases[i].func_, table->children_[0]->value_);
    EXPECT_EQ(T_VARCHAR, table->children_[1]->type_);
  }
  const ParseNode *stmt = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/d/a.csv') as f", stmt));
  const ParseNode *table = find(stmt, T_FILE_TABLE);
  ASSERT_NE(nullptr, table);
  ASSERT_NE(nullptr, table->children_[3]);
  EXPECT_EQ(0, ObString(static_cast<int32_t>(table->children_[3]->str_len_), table->children_[3]->str_value_).compare("f"));
}

TEST_F(TestFileSqlParser, named_options)
{
  const ParseNode *stmt = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_csv('/d/a.csv', header = false, delim = ';', "
                              "sample_size = -1, skip = 2, null = 'NA', ignore_errors = true)", stmt));
  const ParseNode *table = find(stmt, T_FILE_TABLE);
  ASSERT_NE(nullptr, table);
  const ParseNode *options = table->children_[2];
  ASSERT_NE(nullptr, options);
  ASSERT_EQ(6, options->num_child_);
  for (int64_t i = 0; i < options->num_child_; ++i) {
    EXPECT_EQ(T_FILE_TABLE_OPTION, options->children_[i]->type_);
  }
  EXPECT_EQ(-1, options->children_[2]->children_[1]->value_);
  EXPECT_EQ(2, options->children_[3]->children_[1]->value_);
  ASSERT_EQ(OB_SUCCESS, parse("select * from read_json('/d/a.jsonl', format = 'newline_delimited')", stmt));
  ASSERT_NE(nullptr, find(stmt, T_FILE_TABLE_OPTION));
  // a second positional argument is not part of the syntax
  EXPECT_NE(OB_SUCCESS, parse("select * from read_csv('/d/a.csv', 'xml')", stmt));
  EXPECT_NE(OB_SUCCESS, parse("select * from read_csv(path)", stmt));
}

TEST_F(TestFileSqlParser, function_names_stay_identifiers)
{
  const ParseNode *stmt = nullptr;
  EXPECT_EQ(OB_SUCCESS, parse("create table glob(read_csv int, read_parquet int)", stmt));
  EXPECT_EQ(OB_SUCCESS, parse("select read_json from read_ndjson", stmt));
  EXPECT_EQ(nullptr, find(stmt, T_FILE_TABLE));
}

TEST_F(TestFileSqlParser, describe_query)
{
  const ParseNode *stmt = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse("describe select * from read_csv('/d/a.csv')", stmt));
  ASSERT_EQ(T_EXPLAIN, stmt->type_);
  EXPECT_EQ(1u, stmt->reserved_);
  ASSERT_EQ(OB_SUCCESS, parse("desc select 1", stmt));
  EXPECT_EQ(1u, stmt->reserved_);
  ASSERT_EQ(OB_SUCCESS, parse("explain select * from read_csv('/d/a.csv')", stmt));
  ASSERT_EQ(T_EXPLAIN, stmt->type_);
  EXPECT_EQ(0u, stmt->reserved_);
}

TEST_F(TestFileSqlParser, copy_to)
{
  const ParseNode *stmt = nullptr;
  ASSERT_EQ(OB_SUCCESS, parse("copy (select id, name from read_csv('/d/a.csv') order by id) "
                              "to '/d/out.csv' (format csv, header, delimiter '|', quote '\"', "
                              "escape '\\\\', nullstr 'NULL', null '-')", stmt));
  ASSERT_EQ(T_COPY_TO, stmt->type_);
  ASSERT_EQ(4, stmt->num_child_);
  ASSERT_NE(nullptr, stmt->children_[0]);
  EXPECT_EQ(0, ObString(static_cast<int32_t>(stmt->children_[0]->str_len_), stmt->children_[0]->str_value_)
               .compare("select id, name from read_csv('/d/a.csv') order by id"));
  EXPECT_EQ(nullptr, stmt->children_[1]);
  EXPECT_EQ(T_VARCHAR, stmt->children_[2]->type_);
  ASSERT_NE(nullptr, stmt->children_[3]);
  EXPECT_EQ(7, stmt->children_[3]->num_child_);
  const ParseNode *header = stmt->children_[3]->children_[1];
  EXPECT_EQ(T_COPY_OPTION, header->type_);
  EXPECT_EQ(nullptr, header->children_[1]);

  ASSERT_EQ(OB_SUCCESS, parse("copy db1.t1 to '/d/t1.csv'", stmt));
  ASSERT_EQ(T_COPY_TO, stmt->type_);
  EXPECT_EQ(nullptr, stmt->children_[0]);
  ASSERT_NE(nullptr, stmt->children_[1]);
  EXPECT_EQ(T_RELATION_FACTOR, stmt->children_[1]->type_);
  EXPECT_EQ(nullptr, stmt->children_[3]);
}

} // namespace sql
} // namespace oceanbase
