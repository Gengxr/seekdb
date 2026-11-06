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

#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_parquet_reader.h"
#include "share/ob_errno.h"
#include <gtest/gtest.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>

namespace oceanbase
{
namespace sql
{
using namespace common;

class TestFileScanUtils : public ::testing::Test
{
protected:
  void SetUp() override
  {
    char tmpl[] = "/tmp/test_file_scan_utils_XXXXXX";
    ASSERT_NE(nullptr, mkdtemp(tmpl));
    dir_ = tmpl;
  }
  void TearDown() override
  {
    std::string cmd = "rm -rf " + dir_;
    ASSERT_EQ(0, system(cmd.c_str()));
  }
  std::string write_file(const char *name, const std::string &content)
  {
    std::string path = dir_ + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    EXPECT_NE(nullptr, f);
    EXPECT_EQ(content.size(), fwrite(content.data(), 1, content.size(), f));
    fclose(f);
    return path;
  }
  int infer(const std::string &path, const ObFileFormat format, ObFileScanOptions &options,
            ObIArray<ObFileColumnDesc> &columns)
  {
    options.format_ = format;
    options.path_ = ObString(static_cast<int32_t>(path.size()), path.c_str());
    return ObFileSchemaInferer::infer(allocator_, options, columns);
  }
  // all rows of a file as text, one string per row
  int read_all(const ObFileScanOptions &options, const ObIArray<ObFileColumnDesc> &columns,
               std::vector<std::string> &rows)
  {
    int ret = OB_SUCCESS;
    ObFileRowReader reader(allocator_);
    ObSEArray<bool, 16> needed;
    ObSEArray<ObFileValue, 16> values;
    for (int64_t i = 0; i < columns.count(); ++i) {
      needed.push_back(true);
    }
    if (OB_SUCC(reader.open(options, columns, needed))) {
      while (OB_SUCC(reader.get_next_row(values))) {
        std::string row;
        for (int64_t i = 0; i < values.count(); ++i) {
          char buf[64];
          if (i > 0) {
            row += "|";
          }
          if (values.at(i).is_null_) {
            row += "NULL";
          } else if (ObFileColType::VARCHAR == columns.at(i).type_) {
            row += std::string(values.at(i).str_.ptr(), values.at(i).str_.length());
          } else if (ObFileColType::DOUBLE == columns.at(i).type_) {
            snprintf(buf, sizeof(buf), "%g", values.at(i).double_);
            row += buf;
          } else {
            snprintf(buf, sizeof(buf), "%ld", values.at(i).int_);
            row += buf;
          }
        }
        rows.push_back(row);
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
    }
    return ret;
  }
  std::string dir_;
  ObArenaAllocator allocator_;
};

TEST_F(TestFileScanUtils, classify_and_widen)
{
  EXPECT_EQ(ObFileColType::BOOLEAN, ObFileSqlUtil::classify_text(ObString::make_string("TRUE")));
  EXPECT_EQ(ObFileColType::BIGINT, ObFileSqlUtil::classify_text(ObString::make_string("-9223372036854775808")));
  EXPECT_EQ(ObFileColType::DOUBLE, ObFileSqlUtil::classify_text(ObString::make_string("9223372036854775808")));
  EXPECT_EQ(ObFileColType::DOUBLE, ObFileSqlUtil::classify_text(ObString::make_string("1.5e3")));
  EXPECT_EQ(ObFileColType::DATE, ObFileSqlUtil::classify_text(ObString::make_string("2024-02-29")));
  EXPECT_EQ(ObFileColType::VARCHAR, ObFileSqlUtil::classify_text(ObString::make_string("2023-02-29")));
  EXPECT_EQ(ObFileColType::DATETIME, ObFileSqlUtil::classify_text(ObString::make_string("2026-08-01T10:11:12.5")));
  EXPECT_EQ(ObFileColType::VARCHAR, ObFileSqlUtil::classify_text(ObString::make_string("2026-08-01 24:00:00")));
  EXPECT_EQ(ObFileColType::VARCHAR, ObFileSqlUtil::classify_text(ObString::make_string(" 12")));
  EXPECT_EQ(ObFileColType::DOUBLE, ObFileSqlUtil::widen(ObFileColType::BIGINT, ObFileColType::DOUBLE));
  EXPECT_EQ(ObFileColType::DATETIME, ObFileSqlUtil::widen(ObFileColType::DATE, ObFileColType::DATETIME));
  EXPECT_EQ(ObFileColType::VARCHAR, ObFileSqlUtil::widen(ObFileColType::BOOLEAN, ObFileColType::BIGINT));
  EXPECT_EQ(ObFileColType::BIGINT, ObFileSqlUtil::widen(ObFileColType::NULL_TYPE, ObFileColType::BIGINT));
  int32_t days = 0;
  int64_t usec = 0;
  EXPECT_TRUE(ObFileSqlUtil::parse_date(ObString::make_string("1970-01-02"), days));
  EXPECT_EQ(1, days);
  EXPECT_TRUE(ObFileSqlUtil::parse_datetime(ObString::make_string("1970-01-01 00:00:01.25"), usec));
  EXPECT_EQ(1250000, usec);
  EXPECT_TRUE(ObFileSqlUtil::is_valid_utf8("\xe4\xb8\xad", 3));
  EXPECT_FALSE(ObFileSqlUtil::is_valid_utf8("\xe4\xb8", 2));
  EXPECT_FALSE(ObFileSqlUtil::is_valid_utf8("\xc0\xaf", 2));
}

TEST_F(TestFileScanUtils, csv_inference_and_reading)
{
  const std::string path = write_file("a.csv",
      "\xef\xbb\xbfid,name,score,id\r\n"
      "1,\"a,\"\"b\"\"\",1.5,7\r\n"
      "\r\n"
      "2,\"two\nlines\",,8\r\n");
  ObFileScanOptions options;
  ObSEArray<ObFileColumnDesc, 16> columns;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::CSV, options, columns));
  ASSERT_EQ(4, columns.count());
  EXPECT_EQ(0, columns.at(0).name_.compare("id"));
  EXPECT_EQ(0, columns.at(3).name_.compare("id__2"));
  EXPECT_EQ(ObFileColType::BIGINT, columns.at(0).type_);
  EXPECT_EQ(ObFileColType::VARCHAR, columns.at(1).type_);
  EXPECT_EQ(ObFileColType::DOUBLE, columns.at(2).type_);
  EXPECT_TRUE(columns.at(2).nullable_);
  EXPECT_FALSE(columns.at(0).nullable_);
  EXPECT_EQ(2, options.estimated_rows_);
  std::vector<std::string> rows;
  ASSERT_EQ(OB_SUCCESS, read_all(options, columns, rows));
  ASSERT_EQ(2u, rows.size());
  EXPECT_EQ("1|a,\"b\"|1.5|7", rows[0]);
  EXPECT_EQ("2|two\nlines|NULL|8", rows[1]);
}

TEST_F(TestFileScanUtils, csv_options_and_bad_rows)
{
  const std::string path = write_file("b.csv", "x;y\n1;NA\n2\n3;'q;\\'r'\n");
  ObFileScanOptions options;
  ObSEArray<ObFileColumnDesc, 16> columns;
  options.delim_ = ';';
  options.quote_ = '\'';
  options.escape_ = '\\';
  options.has_null_str_ = true;
  options.null_str_ = ObString::make_string("NA");
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(path, ObFileFormat::CSV, options, columns));
  options.ignore_errors_ = true;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::CSV, options, columns));
  std::vector<std::string> rows;
  ASSERT_EQ(OB_SUCCESS, read_all(options, columns, rows));
  ASSERT_EQ(2u, rows.size());
  EXPECT_EQ("1|NULL", rows[0]);
  EXPECT_EQ("3|q;'r", rows[1]);

  const std::string unterminated = write_file("c.csv", "a\n\"open\n");
  ObFileScanOptions options2;
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(unterminated, ObFileFormat::CSV, options2, columns));
}

TEST_F(TestFileScanUtils, json_union_and_conflicts)
{
  const std::string path = write_file("a.jsonl",
      "{\"b\":1,\"a\":\"x\"}\n"
      "\n"
      "{\"a\":\"y\",\"b\":2.5,\"c\":true}\n"
      "{\"b\":\"text\",\"d\":null}\n");
  ObFileScanOptions options;
  ObSEArray<ObFileColumnDesc, 16> columns;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::JSON, options, columns));
  ASSERT_EQ(4, columns.count());
  // keys keep the order in which they first appear
  EXPECT_EQ(0, columns.at(0).name_.compare("b"));
  EXPECT_EQ(0, columns.at(1).name_.compare("a"));
  EXPECT_EQ(ObFileColType::VARCHAR, columns.at(0).type_);
  EXPECT_EQ(ObFileColType::BOOLEAN, columns.at(2).type_);
  EXPECT_EQ(ObFileColType::NULL_TYPE, columns.at(3).type_);
  EXPECT_TRUE(columns.at(1).nullable_);
  std::vector<std::string> rows;
  ASSERT_EQ(OB_SUCCESS, read_all(options, columns, rows));
  ASSERT_EQ(3u, rows.size());
  EXPECT_EQ("1|x|NULL|NULL", rows[0]);
  EXPECT_EQ("2.5|y|1|NULL", rows[1]);
  EXPECT_EQ("text|NULL|NULL|NULL", rows[2]);

  const std::string nested = write_file("b.jsonl", "{\"a\":{\"x\":1}}\n");
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(nested, ObFileFormat::JSON, options, columns));
  const std::string broken = write_file("c.jsonl", "{\"a\":1}\n{\"a\":\n");
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(broken, ObFileFormat::JSON, options, columns));
}

TEST_F(TestFileScanUtils, file_change_is_detected)
{
  const std::string path = write_file("d.csv", "a\n1\n2\n");
  ObFileScanOptions options;
  ObSEArray<ObFileColumnDesc, 16> columns;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::CSV, options, columns));
  usleep(10 * 1000);
  write_file("d.csv", "a\n1\n2\n3\n");
  std::vector<std::string> rows;
  EXPECT_EQ(OB_FILE_SQL_FILE_CHANGED, read_all(options, columns, rows));

  ObFileScanOptions copy;
  ObString text;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::CSV, options, columns));
  ASSERT_EQ(OB_SUCCESS, options.serialize(allocator_, text));
  ASSERT_EQ(OB_SUCCESS, copy.deserialize(allocator_, text));
  EXPECT_TRUE(copy.identity_ == options.identity_);
  EXPECT_EQ(0, copy.path_.compare(options.path_));
}

TEST_F(TestFileScanUtils, snappy)
{
  // "abcabcabcabc": literal "abc" and a copy of 9 bytes at offset 3 that overlaps its output
  const char compressed[] = {12, 0x08, 'a', 'b', 'c', 0x15, 0x03};
  char out[12];
  int64_t length = 0;
  ASSERT_EQ(OB_SUCCESS, ObSnappyDecoder::get_uncompressed_length(compressed, sizeof(compressed), length));
  ASSERT_EQ(12, length);
  ASSERT_EQ(OB_SUCCESS, ObSnappyDecoder::decompress(compressed, sizeof(compressed), out, sizeof(out)));
  EXPECT_EQ(0, memcmp(out, "abcabcabcabc", 12));
  const char bad[] = {12, 0x08, 'a', 'b', 'c', 0x15, 0x09};
  EXPECT_NE(OB_SUCCESS, ObSnappyDecoder::decompress(bad, sizeof(bad), out, sizeof(out)));
}

TEST_F(TestFileScanUtils, parquet_footer_and_pages)
{
  // one optional INT32 column c1 holding 0..6 in a SNAPPY compressed data page
  static const unsigned char data[] = {
    0x50, 0x41, 0x52, 0x31, 0x15, 0x06, 0x15, 0x3c, 0x15, 0x40, 0x5c, 0x15, 0x0e, 0x15, 0x00, 0x15,
    0x0e, 0x15, 0x00, 0x15, 0x04, 0x15, 0x00, 0x11, 0x1c, 0x36, 0x00, 0x28, 0x04, 0x06, 0x00, 0x00,
    0x00, 0x18, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x7f, 0x1c, 0x6c, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x26, 0x94, 0x01, 0x1c, 0x15, 0x02,
    0x19, 0x25, 0x06, 0x00, 0x19, 0x18, 0x02, 0x63, 0x31, 0x15, 0x02, 0x16, 0x0e, 0x16, 0x88, 0x01,
    0x16, 0x8c, 0x01, 0x26, 0x08, 0x3c, 0x36, 0x00, 0x28, 0x04, 0x06, 0x00, 0x00, 0x00, 0x18, 0x04,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15, 0x04, 0x19, 0x2c, 0x48, 0x04, 0x72, 0x6f, 0x6f,
    0x74, 0x15, 0x02, 0x00, 0x15, 0x02, 0x25, 0x02, 0x18, 0x02, 0x63, 0x31, 0x00, 0x16, 0x0e, 0x19,
    0x1c, 0x19, 0x1c, 0x26, 0x94, 0x01, 0x1c, 0x15, 0x02, 0x19, 0x25, 0x06, 0x00, 0x19, 0x18, 0x02,
    0x63, 0x31, 0x15, 0x02, 0x16, 0x0e, 0x16, 0x88, 0x01, 0x16, 0x8c, 0x01, 0x26, 0x08, 0x3c, 0x36,
    0x00, 0x28, 0x04, 0x06, 0x00, 0x00, 0x00, 0x18, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x16, 0x8c, 0x01, 0x16, 0x0e, 0x00, 0x19, 0x1c, 0x18, 0x0c, 0x41, 0x52, 0x52, 0x4f, 0x57, 0x3a,
    0x73, 0x63, 0x68, 0x65, 0x6d, 0x61, 0x18, 0xbc, 0x01, 0x2f, 0x2f, 0x2f, 0x2f, 0x2f, 0x34, 0x51,
    0x41, 0x41, 0x41, 0x41, 0x51, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x4b, 0x41, 0x41, 0x34,
    0x41, 0x44, 0x41, 0x41, 0x4c, 0x41, 0x41, 0x51, 0x41, 0x43, 0x67, 0x41, 0x41, 0x41, 0x42, 0x51,
    0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x42, 0x42, 0x41, 0x41, 0x4b, 0x41, 0x41, 0x77,
    0x41, 0x41, 0x41, 0x41, 0x49, 0x41, 0x41, 0x51, 0x41, 0x43, 0x67, 0x41, 0x41, 0x41, 0x41, 0x67,
    0x41, 0x41, 0x41, 0x41, 0x49, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x45,
    0x41, 0x41, 0x41, 0x41, 0x55, 0x41, 0x41, 0x41, 0x41, 0x45, 0x41, 0x41, 0x55, 0x41, 0x42, 0x41,
    0x41, 0x44, 0x67, 0x41, 0x50, 0x41, 0x41, 0x51, 0x41, 0x41, 0x41, 0x41, 0x49, 0x41, 0x42, 0x41,
    0x41, 0x41, 0x41, 0x41, 0x59, 0x41, 0x41, 0x41, 0x41, 0x49, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x51, 0x49, 0x63, 0x41, 0x41, 0x41, 0x41, 0x43, 0x41, 0x41, 0x4d, 0x41, 0x41, 0x51,
    0x41, 0x43, 0x77, 0x41, 0x49, 0x41, 0x41, 0x41, 0x41, 0x49, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
    0x41, 0x41, 0x41, 0x45, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x67, 0x41, 0x41, 0x41, 0x47, 0x4d,
    0x78, 0x41, 0x41, 0x41, 0x3d, 0x00, 0x18, 0x2c, 0x41, 0x72, 0x72, 0x6f, 0x77, 0x32, 0x20, 0x2d,
    0x20, 0x4e, 0x61, 0x74, 0x69, 0x76, 0x65, 0x20, 0x52, 0x75, 0x73, 0x74, 0x20, 0x69, 0x6d, 0x70,
    0x6c, 0x65, 0x6d, 0x65, 0x6e, 0x74, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x20, 0x6f, 0x66, 0x20, 0x41,
    0x72, 0x72, 0x6f, 0x77, 0x00, 0x4e, 0x01, 0x00, 0x00, 0x50, 0x41, 0x52, 0x31,
  };
  const std::string path = write_file("simple.parquet",
                                      std::string(reinterpret_cast<const char *>(data), sizeof(data)));
  ObFileScanOptions options;
  ObSEArray<ObFileColumnDesc, 16> columns;
  ASSERT_EQ(OB_SUCCESS, infer(path, ObFileFormat::PARQUET, options, columns));
  ASSERT_EQ(1, columns.count());
  EXPECT_EQ(0, columns.at(0).name_.compare("c1"));
  EXPECT_EQ(ObFileColType::BIGINT, columns.at(0).type_);
  EXPECT_TRUE(columns.at(0).nullable_);
  EXPECT_EQ(7, options.estimated_rows_);
  std::vector<std::string> rows;
  ASSERT_EQ(OB_SUCCESS, read_all(options, columns, rows));
  ASSERT_EQ(7u, rows.size());
  EXPECT_EQ("0", rows[0]);
  EXPECT_EQ("6", rows[6]);

  const std::string bad = write_file("bad.parquet", "PAR1 not a footer PAR1");
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(bad, ObFileFormat::PARQUET, options, columns));
  const std::string text = write_file("text.parquet", "id\n1\n");
  EXPECT_EQ(OB_FILE_SQL_INVALID_DATA, infer(text, ObFileFormat::PARQUET, options, columns));
}

TEST_F(TestFileScanUtils, glob)
{
  write_file("x1.csv", "a\n");
  write_file("x2.csv", "a\n");
  write_file("y.json", "{}\n");
  std::string pattern = dir_ + "/x*.csv";
  ObSEArray<ObString, 4> files;
  ASSERT_EQ(OB_SUCCESS, ob_file_sql_glob(allocator_, ObString::make_string(pattern.c_str()), files));
  ASSERT_EQ(2, files.count());
  EXPECT_LT(files.at(0).compare(files.at(1)), 0);
  std::string none = dir_ + "/nothing*";
  ASSERT_EQ(OB_SUCCESS, ob_file_sql_glob(allocator_, ObString::make_string(none.c_str()), files));
  EXPECT_EQ(0, files.count());
}

} // namespace sql
} // namespace oceanbase
