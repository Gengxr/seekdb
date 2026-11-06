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

#ifndef OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
#define OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_

#include "lib/allocator/page_arena.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "lib/string/ob_sql_string.h"

namespace oceanbase
{
namespace common
{
class ObJsonNode;
}
namespace sql
{

// Reading local CSV, JSONL and Parquet files as tables ("file SQL").
//
// The resolver infers the schema of a file with a full scan; the scan operator reads the file
// again and converts every value to the inferred type. Both passes stream the file through a
// fixed size buffer, only one record (CSV, JSONL) or one page per column (Parquet) is held in
// memory. The identity of the file (size, modification time, inode) is taken during inference
// and checked when the scan starts and ends, so a file that changes in between is reported
// instead of being read with a stale schema.

enum class ObFileFormat : int8_t
{
  INVALID = 0,
  CSV = 1,
  JSON = 2,
  PARQUET = 3,
  GLOB = 4,
};

// The inferred column types, ordered so that a wider numeric or temporal type has a larger value.
enum class ObFileColType : int8_t
{
  NULL_TYPE = 0,
  BOOLEAN = 1,
  BIGINT = 2,
  DOUBLE = 3,
  DATE = 4,
  DATETIME = 5,
  VARCHAR = 6,
};

struct ObFileSqlLimits
{
  static const int64_t MAX_COLUMNS = 4096;
  static const int64_t MAX_RECORD_SIZE = 64L * 1024 * 1024;     // one CSV record or JSON line
  static const int64_t MAX_VARCHAR_LENGTH = 256L * 1024;        // longest value of a VARCHAR column
  static const int64_t MAX_PAGE_SIZE = 256L * 1024 * 1024;      // one Parquet page
  static const int64_t MAX_FOOTER_SIZE = 64L * 1024 * 1024;
  static const int64_t MAX_GLOB_FILES = 100000;
  static const int64_t READ_BUFFER_SIZE = 256L * 1024;
};

class ObFileSqlUtil
{
public:
  static const char *type_name(const ObFileColType type);
  // The type of a column that holds values of both types.
  static ObFileColType widen(const ObFileColType a, const ObFileColType b);
  // The narrowest type of a non null text value: BOOLEAN (true/false), BIGINT, DOUBLE, DATE,
  // DATETIME or VARCHAR.
  static ObFileColType classify_text(const common::ObString &text);
  // The type of a JSON string: DATE and DATETIME are recognized, any other string is VARCHAR.
  static ObFileColType classify_json_string(const common::ObString &text);
  static bool parse_bool(const common::ObString &text, bool &value);
  static bool parse_int64(const common::ObString &text, int64_t &value);
  static bool parse_double(const common::ObString &text, double &value);
  // YYYY-MM-DD, as days since 1970-01-01.
  static bool parse_date(const common::ObString &text, int32_t &days);
  // YYYY-MM-DD HH:MI:SS[.ffffff] (or with T), as microseconds since 1970-01-01 00:00:00.
  static bool parse_datetime(const common::ObString &text, int64_t &usec);
  static int64_t days_from_civil(int64_t year, int64_t month, int64_t day);
  static bool is_valid_utf8(const char *ptr, const int64_t len);
  // A file error with a message that names the file and line, as OB_FILE_SQL_INVALID_DATA.
  static int report_error(const common::ObString &path, const int64_t line, const char *fmt, ...)
      __attribute__((format(printf, 3, 4)));
};

struct ObFileIdentity
{
  ObFileIdentity() : size_(-1), mtime_ns_(0), inode_(0) {}
  int load(const char *path);
  bool operator==(const ObFileIdentity &other) const
  {
    return size_ == other.size_ && mtime_ns_ == other.mtime_ns_ && inode_ == other.inode_;
  }
  bool operator!=(const ObFileIdentity &other) const { return !(*this == other); }
  int64_t size_;
  int64_t mtime_ns_;
  int64_t inode_;
  TO_STRING_KV(K_(size), K_(mtime_ns), K_(inode));
};

struct ObFileScanOptions
{
  ObFileScanOptions();
  void reset();
  // A compact text form, kept in the plan and read back by the scan operator.
  int serialize(common::ObIAllocator &allocator, common::ObString &out) const;
  int deserialize(common::ObIAllocator &allocator, const common::ObString &text);

  ObFileFormat format_;
  common::ObString path_;       // the real path of the file, or the pattern of glob()
  bool header_;
  char delim_;
  char quote_;
  char escape_;
  bool has_null_str_;
  common::ObString null_str_;
  int64_t skip_rows_;
  bool ignore_errors_;
  common::ObString secure_file_priv_;   // checked for every file glob() returns
  ObFileIdentity identity_;
  int64_t estimated_rows_;              // rows seen by schema inference, -1 if unknown (not serialized)
  TO_STRING_KV(K_(format), K_(path), K_(header), K_(delim), K_(quote), K_(escape),
               K_(has_null_str), K_(null_str), K_(skip_rows), K_(ignore_errors), K_(identity));
};

struct ObFileColumnDesc
{
  ObFileColumnDesc()
    : name_(), type_(ObFileColType::NULL_TYPE), nullable_(false), max_length_(0), scale_(0),
      src_idx_(-1), src_key_() {}
  common::ObString name_;
  ObFileColType type_;
  bool nullable_;
  int64_t max_length_;    // longest value in bytes, for VARCHAR columns
  int32_t scale_;         // fractional second digits of DATETIME columns
  int64_t src_idx_;       // field index (CSV) or leaf column index (Parquet)
  common::ObString src_key_;  // key of a JSON object
  TO_STRING_KV(K_(name), K_(type), K_(nullable), K_(max_length), K_(scale), K_(src_idx), K_(src_key));
};

typedef common::ObSEArray<ObFileColumnDesc, 16> ObFileColumnArray;

// One value of a row; the column type says which member is valid.
struct ObFileValue
{
  ObFileValue() : is_null_(true), int_(0), double_(0), str_() {}
  void set_null() { is_null_ = true; }
  bool is_null_;
  int64_t int_;    // BOOLEAN, BIGINT, DATE (days), DATETIME (usec)
  double double_;
  common::ObString str_;
};

// Sequential buffered reads of a regular file, limited to the size seen at open.
class ObFileReader
{
public:
  ObFileReader();
  ~ObFileReader() { close(); }
  int open(const common::ObString &path, common::ObIAllocator &allocator);
  void close();
  int rewind();
  // Next byte, OB_ITER_END at the end of the file.
  inline int next_char(char &c)
  {
    int ret = common::OB_SUCCESS;
    if (pos_ >= len_ && OB_FAIL(fill())) {
    } else {
      c = buf_[pos_++];
    }
    return ret;
  }
  inline int peek_char(char &c)
  {
    int ret = common::OB_SUCCESS;
    if (pos_ >= len_ && OB_FAIL(fill())) {
    } else {
      c = buf_[pos_];
    }
    return ret;
  }
  int pread(const int64_t offset, const int64_t size, char *buf);
  const ObFileIdentity &identity() const { return identity_; }
  int64_t file_size() const { return identity_.size_; }
  // Whether the file still has the identity seen at open.
  int check_unchanged() const;
  const common::ObString &path() const { return path_; }

private:
  int fill();
  int fd_;
  char *buf_;
  int64_t pos_;
  int64_t len_;
  int64_t file_offset_;
  ObFileIdentity identity_;
  common::ObString path_;
};

// Splits a CSV file into records. Quoted fields may contain delimiters, quotes (doubled or
// escaped) and line breaks; LF, CRLF and CR end a record; blank lines are skipped.
class ObCsvRecordReader
{
public:
  struct Field
  {
    int64_t offset_;
    int64_t length_;
    bool quoted_;
  };
  ObCsvRecordReader(common::ObIAllocator &allocator);
  ~ObCsvRecordReader() { reset(); }
  int init(ObFileReader *reader, const ObFileScanOptions &options);
  void reset();
  int rewind();
  // OB_ITER_END after the last record.
  int next_record();
  int64_t field_count() const { return fields_.count(); }
  common::ObString field(const int64_t idx) const
  {
    const Field &f = fields_.at(idx);
    return common::ObString(static_cast<int32_t>(f.length_), record_buf_ + f.offset_);
  }
  bool is_quoted(const int64_t idx) const { return fields_.at(idx).quoted_; }
  bool is_null(const int64_t idx) const;
  int64_t line_no() const { return record_line_; }

private:
  int append(const char c);
  int end_field(const bool quoted);
  common::ObIAllocator &allocator_;
  ObFileReader *reader_;
  ObFileScanOptions options_;
  char *record_buf_;
  int64_t record_len_;
  int64_t record_cap_;
  common::ObSEArray<Field, 32> fields_;
  int64_t field_start_;
  int64_t line_;          // line of the next character
  int64_t record_line_;   // first line of the current record
  bool at_start_;
};

// Reads the non blank lines of a JSONL file.
class ObJsonLineReader
{
public:
  ObJsonLineReader(common::ObIAllocator &allocator);
  ~ObJsonLineReader() { reset(); }
  int init(ObFileReader *reader);
  void reset();
  int rewind();
  // Parses the next line into a flat object; OB_ITER_END after the last line.
  int next_object(common::ObIAllocator &tree_allocator, common::ObJsonNode *&object);
  int64_t line_no() const { return record_line_; }

private:
  common::ObIAllocator &allocator_;
  ObFileReader *reader_;
  char *line_buf_;
  int64_t line_len_;
  int64_t line_cap_;
  int64_t line_;
  int64_t record_line_;
};

class ObParquetFile;
class ObParquetColumnReader;

// Reads rows of one file in the inferred schema. needed[i] tells whether column i is read; the
// values of the other columns are NULL.
class ObFileRowReader
{
public:
  ObFileRowReader(common::ObIAllocator &allocator);
  ~ObFileRowReader() { close(); }
  int open(const ObFileScanOptions &options, const common::ObIArray<ObFileColumnDesc> &columns,
           const common::ObIArray<bool> &needed);
  int rescan();
  void close();
  // Values of the next row, one per column; OB_ITER_END after the last row.
  int get_next_row(common::ObIArray<ObFileValue> &values);

private:
  int next_csv_row(common::ObIArray<ObFileValue> &values);
  int next_json_row(common::ObIArray<ObFileValue> &values);
  int next_parquet_row(common::ObIArray<ObFileValue> &values);
  int next_glob_row(common::ObIArray<ObFileValue> &values);
  int convert_text(const ObFileColumnDesc &column, const common::ObString &text,
                   const int64_t line, ObFileValue &value);
  int convert_json(const ObFileColumnDesc &column, common::ObJsonNode *node,
                   const int64_t line, ObFileValue &value);
  int finish_scan();

  common::ObIAllocator &allocator_;
  common::ObArenaAllocator row_allocator_;
  ObFileScanOptions options_;
  common::ObSEArray<ObFileColumnDesc, 16> columns_;
  common::ObSEArray<bool, 16> needed_;
  ObFileReader file_;
  ObCsvRecordReader *csv_;
  ObJsonLineReader *json_;
  ObParquetFile *parquet_;
  common::ObSEArray<ObParquetColumnReader *, 16> parquet_columns_;
  int64_t parquet_row_group_;
  int64_t parquet_rows_left_;
  common::ObSEArray<common::ObString, 16> glob_files_;
  int64_t glob_pos_;
  bool is_open_;
  bool iter_end_;
};

// Schema inference with a full scan of the file. Also records the identity of the file.
class ObFileSchemaInferer
{
public:
  static int infer(common::ObIAllocator &allocator, ObFileScanOptions &options,
                   common::ObIArray<ObFileColumnDesc> &columns);
private:
  static int infer_csv(common::ObIAllocator &allocator, ObFileScanOptions &options,
                       common::ObIArray<ObFileColumnDesc> &columns);
  static int infer_json(common::ObIAllocator &allocator, ObFileScanOptions &options,
                        common::ObIArray<ObFileColumnDesc> &columns);
  static int infer_parquet(common::ObIAllocator &allocator, ObFileScanOptions &options,
                           common::ObIArray<ObFileColumnDesc> &columns);
  static int make_unique_names(common::ObIAllocator &allocator,
                               common::ObIArray<ObFileColumnDesc> &columns);
};

// Snappy raw block decompression (the Parquet SNAPPY codec).
class ObSnappyDecoder
{
public:
  static int get_uncompressed_length(const char *src, const int64_t src_len, int64_t &length);
  static int decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len);
};

// Files that match a glob pattern, sorted; directories are skipped.
int ob_file_sql_glob(common::ObIAllocator &allocator, const common::ObString &pattern,
                     common::ObIArray<common::ObString> &files);

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_ENGINE_BASIC_OB_FILE_SCAN_UTILS_H_
