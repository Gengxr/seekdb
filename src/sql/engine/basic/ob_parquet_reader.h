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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_

#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// A minimal Parquet reader for flat files: the footer (Thrift compact protocol), data pages v1
// and v2, dictionary pages, the codecs UNCOMPRESSED, SNAPPY, GZIP and ZSTD and the encodings
// PLAIN, PLAIN_DICTIONARY, RLE_DICTIONARY, RLE (booleans), DELTA_BINARY_PACKED,
// DELTA_LENGTH_BYTE_ARRAY, DELTA_BYTE_ARRAY and BYTE_STREAM_SPLIT.

enum ObParquetPhysicalType
{
  PQ_BOOLEAN = 0,
  PQ_INT32 = 1,
  PQ_INT64 = 2,
  PQ_INT96 = 3,
  PQ_FLOAT = 4,
  PQ_DOUBLE = 5,
  PQ_BYTE_ARRAY = 6,
  PQ_FIXED_LEN_BYTE_ARRAY = 7,
};

// How a physical value becomes an ObFileValue.
enum ObParquetValueKind
{
  PQ_KIND_BOOL = 0,
  PQ_KIND_INT = 1,           // BIGINT
  PQ_KIND_DOUBLE = 2,
  PQ_KIND_DATE = 3,          // INT32 days
  PQ_KIND_TIMESTAMP = 4,     // INT64 with a unit, or INT96
  PQ_KIND_DECIMAL = 5,       // DOUBLE from an unscaled integer
  PQ_KIND_STRING = 6,
  PQ_KIND_TIME = 7,          // VARCHAR HH:MI:SS[.ffffff]
  PQ_KIND_UUID = 8,          // VARCHAR
  PQ_KIND_FLOAT16 = 9,       // DOUBLE
};

struct ObParquetLeaf
{
  ObParquetLeaf()
    : name_(), physical_type_(PQ_INT32), type_length_(0), nullable_(false), kind_(PQ_KIND_INT),
      scale_(0), time_unit_(1000000), unsigned_(false), file_type_(ObFileColType::BIGINT),
      max_length_(0) {}
  common::ObString name_;
  int32_t physical_type_;
  int32_t type_length_;
  bool nullable_;
  int32_t kind_;
  int32_t scale_;
  int64_t time_unit_;     // values per second of TIMESTAMP and TIME
  bool unsigned_;         // UINT_8 ... UINT_64
  ObFileColType file_type_;
  int64_t max_length_;
  TO_STRING_KV(K_(name), K_(physical_type), K_(nullable), K_(kind), K_(scale), K_(time_unit), K_(file_type));
};

struct ObParquetChunk
{
  ObParquetChunk()
    : physical_type_(0), codec_(0), num_values_(0), start_offset_(0), total_size_(0) {}
  int32_t physical_type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t start_offset_;
  int64_t total_size_;
  TO_STRING_KV(K_(physical_type), K_(codec), K_(num_values), K_(start_offset), K_(total_size));
};

struct ObParquetRowGroup
{
  int64_t num_rows_;
  common::ObSEArray<ObParquetChunk, 16> chunks_;
  TO_STRING_KV(K_(num_rows), K(chunks_.count()));
};

class ObParquetFile
{
public:
  ObParquetFile(common::ObIAllocator &allocator) : allocator_(allocator), file_(nullptr), leaves_(),
      row_groups_(), num_rows_(0) {}
  ~ObParquetFile() { reset(); }
  int open(ObFileReader *file);
  void reset();
  int64_t column_count() const { return leaves_.count(); }
  const ObParquetLeaf &column(const int64_t idx) const { return leaves_.at(idx); }
  int64_t row_group_count() const { return row_groups_.count(); }
  int64_t row_group_rows(const int64_t idx) const { return row_groups_.at(idx)->num_rows_; }
  const ObParquetChunk &chunk(const int64_t row_group, const int64_t column) const
  {
    return row_groups_.at(row_group)->chunks_.at(column);
  }
  const common::ObString &path() const { return file_->path(); }

private:
  int parse_footer(const char *buf, const int64_t len);
  common::ObIAllocator &allocator_;
  ObFileReader *file_;
  common::ObSEArray<ObParquetLeaf, 16> leaves_;
  common::ObSEArray<ObParquetRowGroup *, 4> row_groups_;
  int64_t num_rows_;
};

// The values of one column chunk, page by page.
class ObParquetColumnReader
{
public:
  ObParquetColumnReader();
  ~ObParquetColumnReader() { reset(); }
  int open(ObParquetFile *file, ObFileReader *reader, const int64_t row_group, const int64_t column);
  void reset();
  int next_value(common::ObIAllocator &allocator, ObFileValue &value);

private:
  int load_page();
  int decode_values(const char *data, const int64_t len, const int32_t encoding, const int64_t count,
                    const bool is_dictionary);
  int decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len);
  int convert(common::ObIAllocator &allocator, const int64_t idx, ObFileValue &value);
  void free_page();
  int report(const char *msg);

  ObParquetFile *file_;
  ObFileReader *reader_;
  ObParquetLeaf leaf_;
  ObParquetChunk chunk_;
  int64_t offset_;            // next page header in the file
  int64_t values_read_;       // values (including NULLs) of the chunk returned so far
  // the current page
  char *page_buf_;            // decompressed page
  char *values_buf_;          // decoded values that do not point into the page
  int64_t page_values_;       // values including NULLs
  int64_t page_pos_;
  int64_t value_pos_;         // next non null value
  common::ObArray<uint8_t> def_levels_;
  // non null values of the page: integers (also booleans, dictionary indexes), doubles or strings
  common::ObArray<int64_t> ints_;
  common::ObArray<double> doubles_;
  common::ObArray<common::ObString> strings_;
  bool page_uses_dict_;
  // the dictionary of the chunk
  char *dict_buf_;
  char *dict_values_buf_;
  common::ObArray<int64_t> dict_ints_;
  common::ObArray<double> dict_doubles_;
  common::ObArray<common::ObString> dict_strings_;
  bool has_dict_;
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_PARQUET_READER_H_
