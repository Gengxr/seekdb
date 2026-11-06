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

#define USING_LOG_PREFIX SQL_ENG
#include "sql/engine/basic/ob_file_scan_utils.h"
#include "sql/engine/basic/ob_parquet_reader.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/charset/ob_charset.h"
#include "lib/utility/ob_sort.h"
#include "common/json_type/ob_json_parse.h"
#include "common/json_type/ob_json_tree.h"
#include <fcntl.h>
#include <glob.h>
#include <cmath>
#include <math.h>
#include <stdarg.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
const int64_t USECS_PER_DAY = 86400L * 1000000;

inline bool is_digit(const char c) { return c >= '0' && c <= '9'; }

bool read_digits(const char *&p, const char *end, const int64_t n, int64_t &value)
{
  bool ok = (end - p >= n);
  value = 0;
  for (int64_t i = 0; ok && i < n; ++i, ++p) {
    ok = is_digit(*p);
    value = value * 10 + (*p - '0');
  }
  return ok;
}

bool is_leap_year(const int64_t year)
{
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int64_t days_in_month(const int64_t year, const int64_t month)
{
  static const int64_t DAYS[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (2 == month && is_leap_year(year)) ? 29 : DAYS[month - 1];
}

// YYYY-MM-DD at p, p is moved behind it.
bool parse_date_part(const char *&p, const char *end, int64_t &days)
{
  int64_t year = 0;
  int64_t month = 0;
  int64_t day = 0;
  bool ok = read_digits(p, end, 4, year) && p < end && '-' == *p++
            && read_digits(p, end, 2, month) && p < end && '-' == *p++
            && read_digits(p, end, 2, day);
  if (ok) {
    ok = year >= 1 && month >= 1 && month <= 12 && day >= 1 && day <= days_in_month(year, month);
  }
  if (ok) {
    days = ObFileSqlUtil::days_from_civil(year, month, day);
  }
  return ok;
}

void *file_sql_malloc(const int64_t size)
{
  return ob_malloc(size, ObMemAttr("FileSql"));
}

int grow_buffer(char *&buf, int64_t &cap, const int64_t len, const int64_t need)
{
  int ret = OB_SUCCESS;
  if (need > cap) {
    int64_t new_cap = MAX(cap * 2, 4096);
    while (new_cap < need) {
      new_cap *= 2;
    }
    char *new_buf = static_cast<char *>(file_sql_malloc(new_cap));
    if (OB_ISNULL(new_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (len > 0) {
        MEMCPY(new_buf, buf, len);
      }
      if (OB_NOT_NULL(buf)) {
        ob_free(buf);
      }
      buf = new_buf;
      cap = new_cap;
    }
  }
  return ret;
}

void append_kv(ObSqlString &out, const char key, const ObString &value, int &ret)
{
  if (OB_SUCC(ret)) {
    ret = out.append_fmt("%c%d:", key, value.length());
  }
  if (OB_SUCC(ret) && value.length() > 0) {
    ret = out.append(value);
  }
}

void append_kv_int(ObSqlString &out, const char key, const int64_t value, int &ret)
{
  char buf[32];
  const int64_t len = snprintf(buf, sizeof(buf), "%ld", value);
  append_kv(out, key, ObString(static_cast<int32_t>(len), buf), ret);
}

bool str_to_int(const ObString &str, int64_t &value)
{
  value = 0;
  bool ok = str.length() > 0;
  bool neg = false;
  int64_t i = 0;
  if (ok && '-' == str.ptr()[0]) {
    neg = true;
    i = 1;
    ok = str.length() > 1;
  }
  for (; ok && i < str.length(); ++i) {
    ok = is_digit(str.ptr()[i]);
    value = value * 10 + (str.ptr()[i] - '0');
  }
  if (neg) {
    value = -value;
  }
  return ok;
}
} // namespace

const char *ObFileSqlUtil::type_name(const ObFileColType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case ObFileColType::NULL_TYPE: name = "NULL"; break;
    case ObFileColType::BOOLEAN: name = "BOOLEAN"; break;
    case ObFileColType::BIGINT: name = "BIGINT"; break;
    case ObFileColType::DOUBLE: name = "DOUBLE"; break;
    case ObFileColType::DATE: name = "DATE"; break;
    case ObFileColType::DATETIME: name = "DATETIME"; break;
    case ObFileColType::VARCHAR: name = "VARCHAR"; break;
  }
  return name;
}

ObFileColType ObFileSqlUtil::widen(const ObFileColType a, const ObFileColType b)
{
  ObFileColType type = ObFileColType::VARCHAR;
  if (a == b) {
    type = a;
  } else if (ObFileColType::NULL_TYPE == a) {
    type = b;
  } else if (ObFileColType::NULL_TYPE == b) {
    type = a;
  } else if ((ObFileColType::BIGINT == a || ObFileColType::DOUBLE == a)
             && (ObFileColType::BIGINT == b || ObFileColType::DOUBLE == b)) {
    type = ObFileColType::DOUBLE;
  } else if ((ObFileColType::DATE == a || ObFileColType::DATETIME == a)
             && (ObFileColType::DATE == b || ObFileColType::DATETIME == b)) {
    type = ObFileColType::DATETIME;
  }
  return type;
}

bool ObFileSqlUtil::parse_bool(const ObString &text, bool &value)
{
  bool ok = false;
  if (4 == text.length() && 0 == strncasecmp(text.ptr(), "true", 4)) {
    ok = true;
    value = true;
  } else if (5 == text.length() && 0 == strncasecmp(text.ptr(), "false", 5)) {
    ok = true;
    value = false;
  }
  return ok;
}

bool ObFileSqlUtil::parse_int64(const ObString &text, int64_t &value)
{
  const char *p = text.ptr();
  const char *end = p + text.length();
  bool neg = false;
  bool ok = true;
  uint64_t abs_value = 0;
  if (p < end && ('-' == *p || '+' == *p)) {
    neg = ('-' == *p);
    ++p;
  }
  ok = (p < end);
  for (; ok && p < end; ++p) {
    if (!is_digit(*p)) {
      ok = false;
    } else {
      const uint64_t digit = *p - '0';
      if (abs_value > (UINT64_MAX - digit) / 10) {
        ok = false;
      } else {
        abs_value = abs_value * 10 + digit;
      }
    }
  }
  if (ok) {
    if (neg) {
      ok = abs_value <= static_cast<uint64_t>(INT64_MAX) + 1;
      value = static_cast<int64_t>(0 - abs_value);
    } else {
      ok = abs_value <= static_cast<uint64_t>(INT64_MAX);
      value = static_cast<int64_t>(abs_value);
    }
  }
  return ok;
}

bool ObFileSqlUtil::parse_double(const ObString &text, double &value)
{
  // [+-] digits [. digits] [e [+-] digits], at least one digit in the mantissa
  const char *p = text.ptr();
  const char *end = p + text.length();
  bool ok = false;
  if (p < end && ('-' == *p || '+' == *p)) {
    ++p;
  }
  int64_t mantissa_digits = 0;
  while (p < end && is_digit(*p)) {
    ++p;
    ++mantissa_digits;
  }
  if (p < end && '.' == *p) {
    ++p;
    while (p < end && is_digit(*p)) {
      ++p;
      ++mantissa_digits;
    }
  }
  ok = mantissa_digits > 0;
  if (ok && p < end && ('e' == *p || 'E' == *p)) {
    ++p;
    if (p < end && ('-' == *p || '+' == *p)) {
      ++p;
    }
    int64_t exp_digits = 0;
    while (p < end && is_digit(*p)) {
      ++p;
      ++exp_digits;
    }
    ok = exp_digits > 0;
  }
  ok = ok && p == end && text.length() < 512;
  if (ok) {
    char buf[512];
    MEMCPY(buf, text.ptr(), text.length());
    buf[text.length()] = '\0';
    value = strtod(buf, nullptr);
    ok = std::isfinite(value);
  }
  return ok;
}

int64_t ObFileSqlUtil::days_from_civil(int64_t year, int64_t month, int64_t day)
{
  // proleptic Gregorian calendar, days relative to 1970-01-01
  year -= month <= 2 ? 1 : 0;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const int64_t yoe = year - era * 400;
  const int64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

bool ObFileSqlUtil::parse_date(const ObString &text, int32_t &days)
{
  const char *p = text.ptr();
  const char *end = p + text.length();
  int64_t value = 0;
  bool ok = parse_date_part(p, end, value) && p == end;
  if (ok) {
    days = static_cast<int32_t>(value);
  }
  return ok;
}

bool ObFileSqlUtil::parse_datetime(const ObString &text, int64_t &usec)
{
  const char *p = text.ptr();
  const char *end = p + text.length();
  int64_t days = 0;
  int64_t hour = 0;
  int64_t minute = 0;
  int64_t second = 0;
  int64_t fraction = 0;
  bool ok = parse_date_part(p, end, days) && p < end && (' ' == *p || 'T' == *p);
  if (ok) {
    ++p;
    ok = read_digits(p, end, 2, hour) && p < end && ':' == *p++
         && read_digits(p, end, 2, minute) && p < end && ':' == *p++
         && read_digits(p, end, 2, second)
         && hour < 24 && minute < 60 && second < 60;
  }
  if (ok && p < end) {
    ok = ('.' == *p++);
    int64_t digits = 0;
    while (ok && p < end && is_digit(*p) && digits < 6) {
      fraction = fraction * 10 + (*p++ - '0');
      ++digits;
    }
    ok = ok && digits > 0 && p == end;
    for (; ok && digits < 6; ++digits) {
      fraction *= 10;
    }
  }
  if (ok) {
    usec = days * USECS_PER_DAY + ((hour * 60 + minute) * 60 + second) * 1000000 + fraction;
  }
  return ok;
}

ObFileColType ObFileSqlUtil::classify_text(const ObString &text)
{
  ObFileColType type = ObFileColType::VARCHAR;
  bool bool_value = false;
  int64_t int_value = 0;
  double double_value = 0;
  if (text.empty()) {
  } else if (parse_bool(text, bool_value)) {
    type = ObFileColType::BOOLEAN;
  } else if (parse_int64(text, int_value)) {
    type = ObFileColType::BIGINT;
  } else if (parse_double(text, double_value)) {
    type = ObFileColType::DOUBLE;
  } else {
    type = classify_json_string(text);
  }
  return type;
}

ObFileColType ObFileSqlUtil::classify_json_string(const ObString &text)
{
  ObFileColType type = ObFileColType::VARCHAR;
  int32_t date_value = 0;
  int64_t datetime_value = 0;
  if (text.length() < 10 || !is_digit(text.ptr()[0])) {
  } else if (parse_date(text, date_value)) {
    type = ObFileColType::DATE;
  } else if (parse_datetime(text, datetime_value)) {
    type = ObFileColType::DATETIME;
  }
  return type;
}

bool ObFileSqlUtil::is_valid_utf8(const char *ptr, const int64_t len)
{
  bool ok = true;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(ptr);
  const unsigned char *end = p + len;
  while (ok && p < end) {
    const unsigned char c = *p;
    int64_t extra = 0;
    uint32_t cp = 0;
    if (c < 0x80) {
      ++p;
      continue;
    } else if (c >= 0xC2 && c <= 0xDF) {
      extra = 1;
      cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
      extra = 2;
      cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
      extra = 3;
      cp = c & 0x07;
    } else {
      ok = false;
    }
    if (ok && end - p <= extra) {
      ok = false;
    }
    for (int64_t i = 1; ok && i <= extra; ++i) {
      ok = (p[i] & 0xC0) == 0x80;
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (ok) {
      // no overlong forms, surrogates or values above U+10FFFF
      ok = !((2 == extra && cp < 0x800) || (3 == extra && (cp < 0x10000 || cp > 0x10FFFF))
             || (cp >= 0xD800 && cp <= 0xDFFF));
      p += extra + 1;
    }
  }
  return ok;
}

int ObFileSqlUtil::report_error(const ObString &path, const int64_t line, const char *fmt, ...)
{
  int ret = OB_FILE_SQL_INVALID_DATA;
  char msg[512];
  char buf[1024];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  int64_t len = 0;
  if (line > 0) {
    len = snprintf(buf, sizeof(buf), "file '%.*s', line %ld: %s", path.length(), path.ptr(), line, msg);
  } else {
    len = snprintf(buf, sizeof(buf), "file '%.*s': %s", path.length(), path.ptr(), msg);
  }
  len = MIN(len, static_cast<int64_t>(sizeof(buf) - 1));
  LOG_WARN("invalid file data", K(ret), K(buf));
  LOG_USER_ERROR(OB_FILE_SQL_INVALID_DATA, static_cast<int>(len), buf);
  return ret;
}

int ObFileIdentity::load(const char *path)
{
  int ret = OB_SUCCESS;
  struct stat st;
  if (0 != ::stat(path, &st)) {
    ret = OB_FILE_NOT_EXIST;
  } else {
    size_ = st.st_size;
#ifdef __APPLE__
    mtime_ns_ = st.st_mtimespec.tv_sec * 1000000000L + st.st_mtimespec.tv_nsec;
#else
    mtime_ns_ = st.st_mtim.tv_sec * 1000000000L + st.st_mtim.tv_nsec;
#endif
    inode_ = static_cast<int64_t>(st.st_ino);
  }
  return ret;
}

ObFileScanOptions::ObFileScanOptions()
{
  reset();
}

void ObFileScanOptions::reset()
{
  format_ = ObFileFormat::INVALID;
  path_.reset();
  header_ = true;
  delim_ = ',';
  quote_ = '"';
  escape_ = '"';
  has_null_str_ = false;
  null_str_.reset();
  skip_rows_ = 0;
  ignore_errors_ = false;
  secure_file_priv_.reset();
  identity_ = ObFileIdentity();
  estimated_rows_ = -1;
}

int ObFileScanOptions::serialize(ObIAllocator &allocator, ObString &out) const
{
  int ret = OB_SUCCESS;
  ObSqlString buf;
  append_kv_int(buf, 'f', static_cast<int64_t>(format_), ret);
  append_kv(buf, 'p', path_, ret);
  append_kv_int(buf, 'h', header_ ? 1 : 0, ret);
  append_kv(buf, 'd', ObString(1, &delim_), ret);
  append_kv(buf, 'q', ObString(1, &quote_), ret);
  append_kv(buf, 'e', ObString(1, &escape_), ret);
  if (has_null_str_) {
    append_kv(buf, 'n', null_str_, ret);
  }
  append_kv_int(buf, 's', skip_rows_, ret);
  append_kv_int(buf, 'i', ignore_errors_ ? 1 : 0, ret);
  append_kv(buf, 'v', secure_file_priv_, ret);
  append_kv_int(buf, 'z', identity_.size_, ret);
  append_kv_int(buf, 'm', identity_.mtime_ns_, ret);
  append_kv_int(buf, 'o', identity_.inode_, ret);
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(allocator, buf.string(), out))) {
  }
  return ret;
}

int ObFileScanOptions::deserialize(ObIAllocator &allocator, const ObString &text)
{
  int ret = OB_SUCCESS;
  reset();
  const char *p = text.ptr();
  const char *end = p + text.length();
  while (OB_SUCC(ret) && p < end) {
    const char key = *p++;
    int64_t len = 0;
    while (p < end && is_digit(*p)) {
      len = len * 10 + (*p++ - '0');
    }
    if (p >= end || ':' != *p || end - p - 1 < len) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("invalid file scan options", K(ret), K(text));
    } else {
      ++p;
      ObString value(static_cast<int32_t>(len), p);
      int64_t int_value = 0;
      p += len;
      switch (key) {
        case 'f': str_to_int(value, int_value); format_ = static_cast<ObFileFormat>(int_value); break;
        case 'p': ret = ob_write_string(allocator, value, path_, true); break;
        case 'h': str_to_int(value, int_value); header_ = (1 == int_value); break;
        case 'd': delim_ = len > 0 ? value.ptr()[0] : ','; break;
        case 'q': quote_ = len > 0 ? value.ptr()[0] : '"'; break;
        case 'e': escape_ = len > 0 ? value.ptr()[0] : '"'; break;
        case 'n': has_null_str_ = true; ret = ob_write_string(allocator, value, null_str_); break;
        case 's': str_to_int(value, skip_rows_); break;
        case 'i': str_to_int(value, int_value); ignore_errors_ = (1 == int_value); break;
        case 'v': ret = ob_write_string(allocator, value, secure_file_priv_, true); break;
        case 'z': str_to_int(value, identity_.size_); break;
        case 'm': str_to_int(value, identity_.mtime_ns_); break;
        case 'o': str_to_int(value, identity_.inode_); break;
        default: break;
      }
    }
  }
  return ret;
}

ObFileReader::ObFileReader()
  : fd_(-1), buf_(nullptr), pos_(0), len_(0), file_offset_(0), identity_(), path_()
{
}

int ObFileReader::open(const ObString &path, ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  struct stat st;
  close();
  if (OB_FAIL(ob_write_string(allocator, path, path_, true))) {
  } else if ((fd_ = ::open(path_.ptr(), O_RDONLY | O_CLOEXEC)) < 0) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("failed to open file", K(ret), K(path), K(errno));
    LOG_USER_ERROR(OB_FILE_NOT_EXIST);
  } else if (0 != ::fstat(fd_, &st) || !S_ISREG(st.st_mode)) {
    ret = ObFileSqlUtil::report_error(path_, 0, "not a regular file");
  } else if (OB_FAIL(identity_.load(path_.ptr()))) {
  } else if (OB_ISNULL(buf_ = static_cast<char *>(file_sql_malloc(ObFileSqlLimits::READ_BUFFER_SIZE)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    // the size seen at open bounds every read, data appended later is not read
    identity_.size_ = st.st_size;
    pos_ = 0;
    len_ = 0;
    file_offset_ = 0;
  }
  if (OB_FAIL(ret)) {
    close();
  }
  return ret;
}

void ObFileReader::close()
{
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (OB_NOT_NULL(buf_)) {
    ob_free(buf_);
    buf_ = nullptr;
  }
  pos_ = 0;
  len_ = 0;
  file_offset_ = 0;
}

int ObFileReader::rewind()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else {
    pos_ = 0;
    len_ = 0;
    file_offset_ = 0;
  }
  return ret;
}

int ObFileReader::fill()
{
  int ret = OB_SUCCESS;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (file_offset_ >= identity_.size_) {
    ret = OB_ITER_END;
  } else {
    const bool at_start = (0 == file_offset_);
    const int64_t want = MIN(ObFileSqlLimits::READ_BUFFER_SIZE, identity_.size_ - file_offset_);
    ssize_t n = -1;
    do {
      n = ::pread(fd_, buf_, want, file_offset_);
    } while (n < 0 && EINTR == errno);
    if (n < 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read file", K(ret), K_(path), K(errno));
    } else if (0 == n) {
      ret = OB_ITER_END;
    } else {
      file_offset_ += n;
      len_ = n;
      pos_ = 0;
      // a UTF-8 byte order mark is not part of the data
      if (at_start && n >= 3 && '\xEF' == buf_[0] && '\xBB' == buf_[1] && '\xBF' == buf_[2]) {
        pos_ = 3;
        if (3 == n) {
          ret = fill();
        }
      }
    }
  }
  return ret;
}

int ObFileReader::pread(const int64_t offset, const int64_t size, char *buf)
{
  int ret = OB_SUCCESS;
  int64_t done = 0;
  if (fd_ < 0) {
    ret = OB_NOT_INIT;
  } else if (offset < 0 || size < 0 || offset + size > identity_.size_) {
    ret = ObFileSqlUtil::report_error(path_, 0, "read of %ld bytes at offset %ld is beyond the end of the file",
                                      size, offset);
  }
  while (OB_SUCC(ret) && done < size) {
    ssize_t n = ::pread(fd_, buf + done, size - done, offset + done);
    if (n < 0 && EINTR == errno) {
    } else if (n <= 0) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to read file", K(ret), K_(path), K(errno), K(offset), K(size));
    } else {
      done += n;
    }
  }
  return ret;
}

int ObFileReader::check_unchanged() const
{
  int ret = OB_SUCCESS;
  ObFileIdentity now;
  if (OB_FAIL(now.load(path_.ptr())) || now != identity_) {
    ret = OB_FILE_SQL_FILE_CHANGED;
    LOG_WARN("file changed during the scan", K(ret), K_(path), K_(identity), K(now));
    LOG_USER_ERROR(OB_FILE_SQL_FILE_CHANGED, path_.length(), path_.ptr());
  }
  return ret;
}

ObCsvRecordReader::ObCsvRecordReader(ObIAllocator &allocator)
  : allocator_(allocator), reader_(nullptr), options_(), record_buf_(nullptr), record_len_(0),
    record_cap_(0), fields_(), field_start_(0), line_(1), record_line_(0), at_start_(true)
{
}

int ObCsvRecordReader::init(ObFileReader *reader, const ObFileScanOptions &options)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(reader)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    reader_ = reader;
    options_ = options;
    line_ = 1;
    record_line_ = 0;
  }
  return ret;
}

void ObCsvRecordReader::reset()
{
  if (OB_NOT_NULL(record_buf_)) {
    ob_free(record_buf_);
    record_buf_ = nullptr;
  }
  record_len_ = 0;
  record_cap_ = 0;
  fields_.reset();
  reader_ = nullptr;
}

int ObCsvRecordReader::rewind()
{
  line_ = 1;
  record_line_ = 0;
  return OB_ISNULL(reader_) ? OB_NOT_INIT : reader_->rewind();
}

bool ObCsvRecordReader::is_null(const int64_t idx) const
{
  const Field &f = fields_.at(idx);
  bool null = false;
  if (f.quoted_) {
  } else if (0 == f.length_) {
    null = true;
  } else if (options_.has_null_str_) {
    null = (f.length_ == options_.null_str_.length()
            && 0 == MEMCMP(record_buf_ + f.offset_, options_.null_str_.ptr(), f.length_));
  }
  return null;
}

int ObCsvRecordReader::append(const char c)
{
  int ret = OB_SUCCESS;
  if (record_len_ >= ObFileSqlLimits::MAX_RECORD_SIZE) {
    ret = ObFileSqlUtil::report_error(reader_->path(), record_line_,
                                      "record is longer than %ld bytes", ObFileSqlLimits::MAX_RECORD_SIZE);
  } else if (record_len_ >= record_cap_
             && OB_FAIL(grow_buffer(record_buf_, record_cap_, record_len_, record_len_ + 1))) {
  } else {
    record_buf_[record_len_++] = c;
  }
  return ret;
}

int ObCsvRecordReader::end_field(const bool quoted)
{
  int ret = OB_SUCCESS;
  Field field;
  field.offset_ = field_start_;
  field.length_ = record_len_ - field_start_;
  field.quoted_ = quoted;
  if (fields_.count() >= ObFileSqlLimits::MAX_COLUMNS) {
    ret = ObFileSqlUtil::report_error(reader_->path(), record_line_,
                                      "record has more than %ld fields", ObFileSqlLimits::MAX_COLUMNS);
  } else if (OB_FAIL(fields_.push_back(field))) {
  } else {
    field_start_ = record_len_;
  }
  return ret;
}

int ObCsvRecordReader::next_record()
{
  enum State { FIELD_START, UNQUOTED, QUOTED, AFTER_QUOTE };
  int ret = OB_SUCCESS;
  bool done = false;
  bool has_content = false;
  bool skip_to_line_end = false;   // recovering from a malformed record
  State state = FIELD_START;
  const char delim = options_.delim_;
  const char quote = options_.quote_;
  const char escape = options_.escape_;
  fields_.reuse();
  record_len_ = 0;
  field_start_ = 0;
  record_line_ = line_;
  if (OB_ISNULL(reader_)) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && !done) {
    char c = 0;
    if (OB_FAIL(reader_->next_char(c))) {
      if (OB_ITER_END != ret) {
      } else if (QUOTED == state && !skip_to_line_end) {
        ret = ObFileSqlUtil::report_error(reader_->path(), record_line_, "unterminated quoted field");
      } else if (has_content && !skip_to_line_end) {
        ret = end_field(AFTER_QUOTE == state);
        done = true;
      }
    } else if ('\n' == c || '\r' == c) {
      if ('\r' == c) {
        char next = 0;
        if (OB_SUCCESS == reader_->peek_char(next) && '\n' == next) {
          reader_->next_char(next);
          if (QUOTED == state && !skip_to_line_end) {
            ret = append('\r');
            c = '\n';
          }
        }
      }
      ++line_;
      if (OB_FAIL(ret)) {
      } else if (QUOTED == state && !skip_to_line_end) {
        ret = append(c);
      } else if (skip_to_line_end) {
        // the malformed record ends here, read the next one
        skip_to_line_end = false;
        has_content = false;
        state = FIELD_START;
        fields_.reuse();
        record_len_ = 0;
        field_start_ = 0;
        record_line_ = line_;
      } else if (!has_content) {
        record_line_ = line_;   // blank line
      } else {
        ret = end_field(AFTER_QUOTE == state);
        done = true;
      }
    } else if (skip_to_line_end) {
    } else {
      has_content = true;
      switch (state) {
        case FIELD_START:
          if (c == quote) {
            state = QUOTED;
          } else if (c == delim) {
            ret = end_field(false);
          } else {
            ret = append(c);
            state = UNQUOTED;
          }
          break;
        case UNQUOTED:
          if (c == delim) {
            ret = end_field(false);
            state = FIELD_START;
          } else {
            ret = append(c);
          }
          break;
        case QUOTED:
          if (c == escape && escape != quote) {
            char next = 0;
            if (OB_FAIL(reader_->next_char(next))) {
              if (OB_ITER_END == ret) {
                ret = ObFileSqlUtil::report_error(reader_->path(), record_line_, "unterminated quoted field");
              }
            } else {
              ret = append(next);
            }
          } else if (c == quote) {
            char next = 0;
            if (escape == quote && OB_SUCCESS == reader_->peek_char(next) && next == quote) {
              reader_->next_char(next);
              ret = append(quote);
            } else {
              state = AFTER_QUOTE;
            }
          } else {
            ret = append(c);
          }
          break;
        case AFTER_QUOTE:
          if (c == delim) {
            ret = end_field(true);
            state = FIELD_START;
          } else if (options_.ignore_errors_) {
            skip_to_line_end = true;
          } else {
            ret = ObFileSqlUtil::report_error(reader_->path(), record_line_,
                                              "unexpected character '%c' after a closing quote", c);
          }
          break;
      }
    }
  }
  return ret;
}

ObJsonLineReader::ObJsonLineReader(ObIAllocator &allocator)
  : allocator_(allocator), reader_(nullptr), line_buf_(nullptr), line_len_(0), line_cap_(0),
    line_(0), record_line_(0)
{
}

int ObJsonLineReader::init(ObFileReader *reader)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(reader)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    reader_ = reader;
    line_ = 0;
  }
  return ret;
}

void ObJsonLineReader::reset()
{
  if (OB_NOT_NULL(line_buf_)) {
    ob_free(line_buf_);
    line_buf_ = nullptr;
  }
  line_cap_ = 0;
  line_len_ = 0;
  reader_ = nullptr;
}

int ObJsonLineReader::rewind()
{
  line_ = 0;
  return OB_ISNULL(reader_) ? OB_NOT_INIT : reader_->rewind();
}

int ObJsonLineReader::next_object(ObIAllocator &tree_allocator, ObJsonNode *&object)
{
  int ret = OB_SUCCESS;
  bool found = false;
  object = nullptr;
  if (OB_ISNULL(reader_)) {
    ret = OB_NOT_INIT;
  }
  while (OB_SUCC(ret) && !found) {
    bool line_end = false;
    bool blank = true;
    line_len_ = 0;
    ++line_;
    while (OB_SUCC(ret) && !line_end) {
      char c = 0;
      if (OB_FAIL(reader_->next_char(c))) {
        if (OB_ITER_END == ret && line_len_ > 0) {
          ret = OB_SUCCESS;
          line_end = true;
        }
      } else if ('\n' == c) {
        line_end = true;
      } else if (line_len_ >= ObFileSqlLimits::MAX_RECORD_SIZE) {
        ret = ObFileSqlUtil::report_error(reader_->path(), line_, "line is longer than %ld bytes",
                                          ObFileSqlLimits::MAX_RECORD_SIZE);
      } else if (line_len_ >= line_cap_
                 && OB_FAIL(grow_buffer(line_buf_, line_cap_, line_len_, line_len_ + 1))) {
      } else {
        line_buf_[line_len_++] = c;
        blank = blank && (' ' == c || '\t' == c || '\r' == c);
      }
    }
    if (OB_SUCC(ret) && !blank) {
      const char *syntax_error = nullptr;
      uint64_t offset = 0;
      found = true;
      record_line_ = line_;
      if (!ObFileSqlUtil::is_valid_utf8(line_buf_, line_len_)) {
        ret = ObFileSqlUtil::report_error(reader_->path(), line_, "invalid UTF-8");
      } else if (OB_FAIL(ObJsonParser::parse_json_text(&tree_allocator, line_buf_, line_len_,
                                                       syntax_error, &offset, object))) {
        ret = ObFileSqlUtil::report_error(reader_->path(), line_, "invalid JSON at offset %lu: %s",
                                          offset, OB_ISNULL(syntax_error) ? "syntax error" : syntax_error);
      } else if (OB_ISNULL(object) || ObJsonNodeType::J_OBJECT != object->json_type()) {
        ret = ObFileSqlUtil::report_error(reader_->path(), line_, "a line must hold one JSON object");
      }
    }
  }
  return ret;
}

namespace
{
// Type of a JSON value; nested values are rejected.
int classify_json(const ObString &path, const int64_t line, const ObString &key,
                  ObJsonNode *node, ObFileColType &type)
{
  int ret = OB_SUCCESS;
  type = ObFileColType::NULL_TYPE;
  switch (node->json_type()) {
    case ObJsonNodeType::J_NULL:
      break;
    case ObJsonNodeType::J_BOOLEAN:
      type = ObFileColType::BOOLEAN;
      break;
    case ObJsonNodeType::J_INT:
      type = ObFileColType::BIGINT;
      break;
    case ObJsonNodeType::J_UINT:
      type = node->get_uint() <= static_cast<uint64_t>(INT64_MAX) ? ObFileColType::BIGINT
                                                                    : ObFileColType::DOUBLE;
      break;
    case ObJsonNodeType::J_DOUBLE:
    case ObJsonNodeType::J_DECIMAL:
    case ObJsonNodeType::J_OFLOAT:
    case ObJsonNodeType::J_ODOUBLE:
      type = ObFileColType::DOUBLE;
      break;
    case ObJsonNodeType::J_STRING:
      type = ObFileSqlUtil::classify_json_string(ObString(static_cast<int32_t>(node->get_data_length()),
                                                          node->get_data()));
      break;
    case ObJsonNodeType::J_OBJECT:
    case ObJsonNodeType::J_ARRAY:
      ret = ObFileSqlUtil::report_error(path, line, "nested value of key '%.*s' is not supported",
                                        key.length(), key.ptr());
      break;
    default:
      type = ObFileColType::VARCHAR;
      break;
  }
  return ret;
}

int64_t find_column(const ObIArray<ObFileColumnDesc> &columns, const ObString &key, const int64_t hint)
{
  int64_t idx = -1;
  if (hint >= 0 && hint < columns.count()
      && ObCharset::case_insensitive_equal(columns.at(hint).src_key_, key)) {
    idx = hint;
  }
  for (int64_t i = 0; -1 == idx && i < columns.count(); ++i) {
    if (ObCharset::case_insensitive_equal(columns.at(i).src_key_, key)) {
      idx = i;
    }
  }
  return idx;
}

void update_column(ObFileColumnDesc &column, const ObFileColType type, const ObString &text,
                   const int64_t length)
{
  if (ObFileColType::NULL_TYPE == type) {
    column.nullable_ = true;
  } else {
    column.type_ = ObFileSqlUtil::widen(column.type_, type);
    column.max_length_ = MAX(column.max_length_, length);
    if (ObFileColType::DATETIME == type) {
      const char *dot = static_cast<const char *>(memchr(text.ptr(), '.', text.length()));
      if (OB_NOT_NULL(dot)) {
        column.scale_ = MAX(column.scale_, static_cast<int32_t>(text.ptr() + text.length() - dot - 1));
      }
    }
  }
}
} // namespace

int ObFileSchemaInferer::infer(ObIAllocator &allocator, ObFileScanOptions &options,
                               ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  columns.reuse();
  switch (options.format_) {
    case ObFileFormat::CSV:
      ret = infer_csv(allocator, options, columns);
      break;
    case ObFileFormat::JSON:
      ret = infer_json(allocator, options, columns);
      break;
    case ObFileFormat::PARQUET:
      ret = infer_parquet(allocator, options, columns);
      break;
    case ObFileFormat::GLOB: {
      ObFileColumnDesc column;
      column.name_ = ObString::make_string("file");
      column.src_key_ = column.name_;
      column.type_ = ObFileColType::VARCHAR;
      column.max_length_ = OB_MAX_FILE_NAME_LENGTH;
      column.src_idx_ = 0;
      ret = columns.push_back(column);
      break;
    }
    default:
      ret = OB_ERR_UNEXPECTED;
      break;
  }
  if (OB_SUCC(ret) && columns.empty()) {
    ret = ObFileSqlUtil::report_error(options.path_, 0, "no columns found");
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (ObFileColType::VARCHAR == columns.at(i).type_
        && columns.at(i).max_length_ > ObFileSqlLimits::MAX_VARCHAR_LENGTH) {
      ret = ObFileSqlUtil::report_error(options.path_, 0, "a value of column '%.*s' is longer than %ld bytes",
                                        columns.at(i).name_.length(), columns.at(i).name_.ptr(),
                                        ObFileSqlLimits::MAX_VARCHAR_LENGTH);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(make_unique_names(allocator, columns))) {
  }
  return ret;
}

int ObFileSchemaInferer::make_unique_names(ObIAllocator &allocator, ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    ObFileColumnDesc &column = columns.at(i);
    if (column.name_.empty()) {
      char buf[32];
      const int64_t len = snprintf(buf, sizeof(buf), "column%ld", i);
      ret = ob_write_string(allocator, ObString(static_cast<int32_t>(len), buf), column.name_);
    }
    // a repeated name gets the suffix __2, __3, ... (column names are case insensitive)
    for (int64_t suffix = 2; OB_SUCC(ret); ++suffix) {
      bool duplicated = false;
      for (int64_t j = 0; !duplicated && j < i; ++j) {
        duplicated = ObCharset::case_insensitive_equal(columns.at(j).name_, column.name_);
      }
      if (!duplicated) {
        break;
      }
      ObSqlString name;
      const ObString &base = OB_ISNULL(column.src_key_.ptr()) || column.src_key_.empty()
                             ? column.name_ : column.src_key_;
      if (OB_FAIL(name.append_fmt("%.*s__%ld", base.length(), base.ptr(), suffix))) {
      } else if (OB_FAIL(ob_write_string(allocator, name.string(), column.name_))) {
      }
    }
    if (OB_SUCC(ret) && column.name_.length() > OB_MAX_COLUMN_NAME_LENGTH) {
      ret = ObFileSqlUtil::report_error(ObString(), 0, "column name '%.*s' is too long",
                                        column.name_.length(), column.name_.ptr());
    }
  }
  return ret;
}

int ObFileSchemaInferer::infer_csv(ObIAllocator &allocator, ObFileScanOptions &options,
                                   ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  ObFileReader file;
  ObCsvRecordReader csv(allocator);
  bool first = true;
  int64_t skipped = 0;
  int64_t data_rows = 0;
  if (OB_FAIL(file.open(options.path_, allocator))) {
  } else if (OB_FAIL(csv.init(&file, options))) {
  } else {
    options.identity_ = file.identity();
  }
  while (OB_SUCC(ret)) {
    if (OB_FAIL(csv.next_record())) {
    } else if (skipped < options.skip_rows_) {
      ++skipped;
    } else if (first) {
      first = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < csv.field_count(); ++i) {
        ObFileColumnDesc column;
        column.src_idx_ = i;
        if (options.header_) {
          const ObString name = csv.field(i);
          if (!ObFileSqlUtil::is_valid_utf8(name.ptr(), name.length())) {
            ret = ObFileSqlUtil::report_error(options.path_, csv.line_no(), "invalid UTF-8 in the header");
          } else if (OB_FAIL(ob_write_string(allocator, name, column.src_key_))) {
          } else {
            column.name_ = column.src_key_;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(columns.push_back(column))) {
        }
      }
      if (OB_SUCC(ret) && !options.header_) {
        // without a header the first record is data
        first = false;
        csv.rewind();
        for (int64_t i = 0; OB_SUCC(ret) && i < options.skip_rows_; ++i) {
          ret = csv.next_record();
        }
      }
    } else if (csv.field_count() != columns.count()) {
      if (!options.ignore_errors_) {
        ret = ObFileSqlUtil::report_error(options.path_, csv.line_no(), "expected %ld fields but found %ld",
                                          columns.count(), csv.field_count());
      }
    } else {
      ++data_rows;
      for (int64_t i = 0; OB_SUCC(ret) && i < csv.field_count(); ++i) {
        const ObString value = csv.field(i);
        if (csv.is_null(i)) {
          update_column(columns.at(i), ObFileColType::NULL_TYPE, value, 0);
        } else if (!ObFileSqlUtil::is_valid_utf8(value.ptr(), value.length())) {
          ret = ObFileSqlUtil::report_error(options.path_, csv.line_no(), "invalid UTF-8 in field %ld", i + 1);
        } else {
          update_column(columns.at(i), ObFileSqlUtil::classify_text(value), value, value.length());
        }
      }
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
    options.estimated_rows_ = data_rows;
  }
  if (OB_SUCC(ret) && OB_FAIL(file.check_unchanged())) {
  }
  return ret;
}

int ObFileSchemaInferer::infer_json(ObIAllocator &allocator, ObFileScanOptions &options,
                                    ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  ObFileReader file;
  ObJsonLineReader json(allocator);
  ObArenaAllocator tree_allocator("FileSqlJson");
  ObSEArray<bool, 16> seen;
  ObSEArray<std::pair<const char *, int64_t>, 16> order;
  int64_t rows = 0;
  if (OB_FAIL(file.open(options.path_, allocator))) {
  } else if (OB_FAIL(json.init(&file))) {
  } else {
    options.identity_ = file.identity();
  }
  while (OB_SUCC(ret)) {
    ObJsonNode *object = nullptr;
    tree_allocator.reuse();
    if (OB_FAIL(json.next_object(tree_allocator, object))) {
    } else {
      seen.reuse();
      order.reuse();
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        ret = seen.push_back(false);
      }
      // the parser sorts the keys of an object; it parses in place, so the position of a key
      // in the line gives the order of the keys in the file
      for (uint64_t i = 0; OB_SUCC(ret) && i < object->element_count(); ++i) {
        ObString key;
        ObJsonNode *value = nullptr;
        if (OB_FAIL(static_cast<ObJsonObject *>(object)->get_value_by_idx(i, key, value))) {
        } else if (OB_FAIL(order.push_back(std::make_pair(key.ptr(), static_cast<int64_t>(i))))) {
        }
      }
      if (OB_SUCC(ret)) {
        lib::ob_sort(order.begin(), order.end());
      }
      for (int64_t k = 0; OB_SUCC(ret) && k < order.count(); ++k) {
        const uint64_t i = static_cast<uint64_t>(order.at(k).second);
        ObString key;
        ObJsonNode *value = nullptr;
        ObFileColType type = ObFileColType::NULL_TYPE;
        int64_t idx = -1;
        if (OB_FAIL(static_cast<ObJsonObject *>(object)->get_value_by_idx(i, key, value))) {
        } else if (OB_ISNULL(value)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(classify_json(options.path_, json.line_no(), key, value, type))) {
        } else if (-1 == (idx = find_column(columns, key, static_cast<int64_t>(i)))) {
          // a field that earlier lines did not have: NULL there
          ObFileColumnDesc column;
          column.src_idx_ = columns.count();
          column.nullable_ = rows > 0;
          if (columns.count() >= ObFileSqlLimits::MAX_COLUMNS) {
            ret = ObFileSqlUtil::report_error(options.path_, json.line_no(), "more than %ld keys",
                                              ObFileSqlLimits::MAX_COLUMNS);
          } else if (OB_FAIL(ob_write_string(allocator, key, column.src_key_))) {
          } else if (FALSE_IT(column.name_ = column.src_key_)) {
          } else if (OB_FAIL(columns.push_back(column))) {
          } else if (OB_FAIL(seen.push_back(false))) {
          } else {
            idx = columns.count() - 1;
          }
        }
        if (OB_SUCC(ret) && idx >= 0) {
          int64_t length = 0;
          ObString text;
          if (ObJsonNodeType::J_STRING == value->json_type()) {
            text.assign_ptr(value->get_data(), static_cast<int32_t>(value->get_data_length()));
            length = text.length();
          } else if (ObFileColType::NULL_TYPE != type) {
            length = 32;   // the text of a number or boolean, if the column becomes VARCHAR
          }
          seen.at(idx) = true;
          update_column(columns.at(idx), type, text, length);
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
        if (!seen.at(i)) {
          columns.at(i).nullable_ = true;
        }
      }
      ++rows;
    }
  }
  if (OB_ITER_END == ret) {
    ret = OB_SUCCESS;
    options.estimated_rows_ = rows;
  }
  // a column with values of different kinds is VARCHAR; its numbers keep their JSON text
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    if (ObFileColType::VARCHAR == columns.at(i).type_) {
      columns.at(i).max_length_ = MAX(columns.at(i).max_length_, 32);
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(file.check_unchanged())) {
  }
  return ret;
}

int ObFileSchemaInferer::infer_parquet(ObIAllocator &allocator, ObFileScanOptions &options,
                                       ObIArray<ObFileColumnDesc> &columns)
{
  int ret = OB_SUCCESS;
  ObFileReader file;
  ObParquetFile parquet(allocator);
  if (OB_FAIL(file.open(options.path_, allocator))) {
  } else if (OB_FAIL(parquet.open(&file))) {
  } else {
    options.identity_ = file.identity();
    options.estimated_rows_ = 0;
    for (int64_t i = 0; i < parquet.row_group_count(); ++i) {
      options.estimated_rows_ += parquet.row_group_rows(i);
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < parquet.column_count(); ++i) {
      const ObParquetLeaf &leaf = parquet.column(i);
      ObFileColumnDesc column;
      column.src_idx_ = i;
      column.type_ = leaf.file_type_;
      column.nullable_ = leaf.nullable_;
      column.max_length_ = leaf.max_length_;
      column.scale_ = 0;
      if (OB_FAIL(ob_write_string(allocator, leaf.name_, column.src_key_))) {
      } else if (FALSE_IT(column.name_ = column.src_key_)) {
      } else if (OB_FAIL(columns.push_back(column))) {
      }
    }
    // DATETIME columns get as many fractional digits as their values use
    for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
      ObFileColumnDesc &column = columns.at(i);
      ObArenaAllocator value_allocator("FileSqlInfer");
      for (int64_t rg = 0; OB_SUCC(ret) && ObFileColType::DATETIME == column.type_ && column.scale_ < 6
           && rg < parquet.row_group_count(); ++rg) {
        ObParquetColumnReader reader;
        if (parquet.row_group_rows(rg) <= 0) {
        } else if (OB_FAIL(reader.open(&parquet, &file, rg, i))) {
        }
        for (int64_t row = 0; OB_SUCC(ret) && row < parquet.row_group_rows(rg) && column.scale_ < 6; ++row) {
          ObFileValue value;
          value_allocator.reuse();
          if (OB_FAIL(reader.next_value(value_allocator, value))) {
          } else if (value.is_null_ || 0 == value.int_ % 1000000) {
          } else if (0 == value.int_ % 1000) {
            column.scale_ = MAX(column.scale_, 3);
          } else {
            column.scale_ = 6;
          }
        }
      }
    }
  }
  return ret;
}

ObFileRowReader::ObFileRowReader(ObIAllocator &allocator)
  : allocator_(allocator), row_allocator_("FileSqlRow"), options_(), columns_(), needed_(),
    file_(), csv_(nullptr), json_(nullptr), parquet_(nullptr), parquet_columns_(),
    parquet_row_group_(-1), parquet_rows_left_(0), glob_files_(), glob_pos_(0),
    is_open_(false), iter_end_(false)
{
}

int ObFileRowReader::open(const ObFileScanOptions &options,
                          const ObIArray<ObFileColumnDesc> &columns,
                          const ObIArray<bool> &needed)
{
  int ret = OB_SUCCESS;
  close();
  options_ = options;
  if (OB_FAIL(columns_.assign(columns))) {
  } else if (OB_FAIL(needed_.assign(needed))) {
  } else if (needed_.count() != columns_.count()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (ObFileFormat::GLOB == options_.format_) {
    if (OB_FAIL(ob_file_sql_glob(allocator_, options_.path_, glob_files_))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < glob_files_.count(); ++i) {
      char real_path[PATH_MAX + 1];
      ObSqlString file;
      if (OB_FAIL(file.append(glob_files_.at(i)))) {
      } else if (OB_ISNULL(::realpath(file.ptr(), real_path))) {
        ret = OB_FILE_NOT_EXIST;
      } else if (OB_FAIL(ObResolverUtils::check_secure_path(options_.secure_file_priv_, real_path))) {
        LOG_WARN("glob result is outside secure_file_priv", K(ret), K(real_path));
      }
    }
    glob_pos_ = 0;
  } else if (OB_FAIL(file_.open(options_.path_, allocator_))) {
  } else if (file_.identity() != options_.identity_) {
    ret = OB_FILE_SQL_FILE_CHANGED;
    LOG_WARN("file changed since its schema was inferred", K(ret), K(file_.identity()), K(options_));
    LOG_USER_ERROR(OB_FILE_SQL_FILE_CHANGED, options_.path_.length(), options_.path_.ptr());
  } else if (ObFileFormat::CSV == options_.format_) {
    void *buf = allocator_.alloc(sizeof(ObCsvRecordReader));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(csv_ = new (buf) ObCsvRecordReader(allocator_))) {
    } else if (OB_FAIL(csv_->init(&file_, options_))) {
    }
  } else if (ObFileFormat::JSON == options_.format_) {
    void *buf = allocator_.alloc(sizeof(ObJsonLineReader));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(json_ = new (buf) ObJsonLineReader(allocator_))) {
    } else if (OB_FAIL(json_->init(&file_))) {
    }
  } else if (ObFileFormat::PARQUET == options_.format_) {
    void *buf = allocator_.alloc(sizeof(ObParquetFile));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (FALSE_IT(parquet_ = new (buf) ObParquetFile(allocator_))) {
    } else if (OB_FAIL(parquet_->open(&file_))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
      ret = parquet_columns_.push_back(nullptr);
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }
  if (OB_SUCC(ret)) {
    is_open_ = true;
    ret = rescan();
  }
  return ret;
}

int ObFileRowReader::rescan()
{
  int ret = OB_SUCCESS;
  iter_end_ = false;
  if (!is_open_) {
    ret = OB_NOT_INIT;
  } else if (ObFileFormat::GLOB == options_.format_) {
    glob_pos_ = 0;
  } else if (OB_NOT_NULL(csv_)) {
    if (OB_FAIL(csv_->rewind())) {
    }
    // skipped rows and the header are not data
    const int64_t skip = options_.skip_rows_ + (options_.header_ ? 1 : 0);
    for (int64_t i = 0; OB_SUCC(ret) && i < skip; ++i) {
      if (OB_FAIL(csv_->next_record()) && OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        iter_end_ = true;
      }
    }
  } else if (OB_NOT_NULL(json_)) {
    ret = json_->rewind();
  } else if (OB_NOT_NULL(parquet_)) {
    parquet_row_group_ = -1;
    parquet_rows_left_ = 0;
  }
  return ret;
}

void ObFileRowReader::close()
{
  if (OB_NOT_NULL(csv_)) {
    csv_->~ObCsvRecordReader();
    csv_ = nullptr;
  }
  if (OB_NOT_NULL(json_)) {
    json_->~ObJsonLineReader();
    json_ = nullptr;
  }
  for (int64_t i = 0; i < parquet_columns_.count(); ++i) {
    if (OB_NOT_NULL(parquet_columns_.at(i))) {
      parquet_columns_.at(i)->~ObParquetColumnReader();
      parquet_columns_.at(i) = nullptr;
    }
  }
  parquet_columns_.reset();
  if (OB_NOT_NULL(parquet_)) {
    parquet_->~ObParquetFile();
    parquet_ = nullptr;
  }
  file_.close();
  glob_files_.reset();
  row_allocator_.reset();
  is_open_ = false;
}

int ObFileRowReader::finish_scan()
{
  int ret = OB_SUCCESS;
  iter_end_ = true;
  if (ObFileFormat::GLOB != options_.format_ && OB_FAIL(file_.check_unchanged())) {
  } else {
    ret = OB_ITER_END;
  }
  return ret;
}

int ObFileRowReader::get_next_row(ObIArray<ObFileValue> &values)
{
  int ret = OB_SUCCESS;
  row_allocator_.reuse();
  if (!is_open_) {
    ret = OB_NOT_INIT;
  } else if (iter_end_) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(values.prepare_allocate(columns_.count()))) {
  } else {
    for (int64_t i = 0; i < values.count(); ++i) {
      values.at(i).set_null();
    }
    switch (options_.format_) {
      case ObFileFormat::CSV: ret = next_csv_row(values); break;
      case ObFileFormat::JSON: ret = next_json_row(values); break;
      case ObFileFormat::PARQUET: ret = next_parquet_row(values); break;
      case ObFileFormat::GLOB: ret = next_glob_row(values); break;
      default: ret = OB_ERR_UNEXPECTED; break;
    }
    if (OB_ITER_END == ret) {
      ret = finish_scan();
    }
  }
  return ret;
}

int ObFileRowReader::convert_text(const ObFileColumnDesc &column, const ObString &text,
                                  const int64_t line, ObFileValue &value)
{
  int ret = OB_SUCCESS;
  bool ok = true;
  value.is_null_ = false;
  switch (column.type_) {
    case ObFileColType::NULL_TYPE:
      ok = false;
      break;
    case ObFileColType::BOOLEAN: {
      bool b = false;
      ok = ObFileSqlUtil::parse_bool(text, b);
      value.int_ = b ? 1 : 0;
      break;
    }
    case ObFileColType::BIGINT:
      ok = ObFileSqlUtil::parse_int64(text, value.int_);
      break;
    case ObFileColType::DOUBLE:
      ok = ObFileSqlUtil::parse_double(text, value.double_);
      break;
    case ObFileColType::DATE: {
      int32_t days = 0;
      ok = ObFileSqlUtil::parse_date(text, days);
      value.int_ = days;
      break;
    }
    case ObFileColType::DATETIME: {
      int32_t days = 0;
      if (ObFileSqlUtil::parse_datetime(text, value.int_)) {
      } else if (ObFileSqlUtil::parse_date(text, days)) {
        value.int_ = days * USECS_PER_DAY;
      } else {
        ok = false;
      }
      break;
    }
    case ObFileColType::VARCHAR:
      value.str_ = text;
      break;
  }
  if (!ok) {
    ret = ObFileSqlUtil::report_error(options_.path_, line, "value '%.*s' of column '%.*s' is not a %s",
                                      MIN(text.length(), 64), text.ptr(),
                                      column.name_.length(), column.name_.ptr(),
                                      ObFileSqlUtil::type_name(column.type_));
  }
  return ret;
}

int ObFileRowReader::next_csv_row(ObIArray<ObFileValue> &values)
{
  int ret = OB_SUCCESS;
  bool found = false;
  while (OB_SUCC(ret) && !found) {
    if (OB_FAIL(csv_->next_record())) {
    } else if (csv_->field_count() != columns_.count()) {
      if (!options_.ignore_errors_) {
        ret = ObFileSqlUtil::report_error(options_.path_, csv_->line_no(), "expected %ld fields but found %ld",
                                          columns_.count(), csv_->field_count());
      }
    } else {
      found = true;
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    const int64_t idx = columns_.at(i).src_idx_;
    if (!needed_.at(i)) {
    } else if (idx < 0 || idx >= csv_->field_count()) {
      ret = OB_ERR_UNEXPECTED;
    } else if (csv_->is_null(idx)) {
      values.at(i).set_null();
    } else if (OB_FAIL(convert_text(columns_.at(i), csv_->field(idx), csv_->line_no(), values.at(i)))) {
    }
  }
  return ret;
}

int ObFileRowReader::convert_json(const ObFileColumnDesc &column, ObJsonNode *node,
                                  const int64_t line, ObFileValue &value)
{
  int ret = OB_SUCCESS;
  bool ok = true;
  const ObJsonNodeType type = node->json_type();
  const bool is_string = ObJsonNodeType::J_STRING == type;
  const ObString str = is_string ? ObString(static_cast<int32_t>(node->get_data_length()), node->get_data())
                                 : ObString();
  if (ObJsonNodeType::J_NULL == type) {
    value.set_null();
  } else {
    value.is_null_ = false;
    switch (column.type_) {
      case ObFileColType::NULL_TYPE:
        ok = false;
        break;
      case ObFileColType::BOOLEAN:
        ok = ObJsonNodeType::J_BOOLEAN == type;
        value.int_ = ok && node->get_boolean() ? 1 : 0;
        break;
      case ObFileColType::BIGINT:
        if (ObJsonNodeType::J_INT == type) {
          value.int_ = node->get_int();
        } else if (ObJsonNodeType::J_UINT == type && node->get_uint() <= static_cast<uint64_t>(INT64_MAX)) {
          value.int_ = static_cast<int64_t>(node->get_uint());
        } else {
          ok = false;
        }
        break;
      case ObFileColType::DOUBLE:
        ok = !is_string && ObJsonNodeType::J_BOOLEAN != type
             && OB_SUCCESS == node->to_double(value.double_);
        break;
      case ObFileColType::DATE: {
        int32_t days = 0;
        ok = is_string && ObFileSqlUtil::parse_date(str, days);
        value.int_ = days;
        break;
      }
      case ObFileColType::DATETIME: {
        int32_t days = 0;
        if (!is_string) {
          ok = false;
        } else if (ObFileSqlUtil::parse_datetime(str, value.int_)) {
        } else if (ObFileSqlUtil::parse_date(str, days)) {
          value.int_ = days * USECS_PER_DAY;
        } else {
          ok = false;
        }
        break;
      }
      case ObFileColType::VARCHAR:
        if (is_string) {
          value.str_ = str;
        } else {
          ObJsonBuffer buf(&row_allocator_);
          if (OB_FAIL(node->print(buf, false))) {
          } else {
            value.str_ = buf.string();
          }
        }
        break;
    }
  }
  if (OB_SUCC(ret) && !ok) {
    ret = ObFileSqlUtil::report_error(options_.path_, line, "value of key '%.*s' is not a %s",
                                      column.src_key_.length(), column.src_key_.ptr(),
                                      ObFileSqlUtil::type_name(column.type_));
  }
  return ret;
}

int ObFileRowReader::next_json_row(ObIArray<ObFileValue> &values)
{
  int ret = OB_SUCCESS;
  ObJsonNode *object = nullptr;
  if (OB_FAIL(json_->next_object(row_allocator_, object))) {
  }
  for (uint64_t i = 0; OB_SUCC(ret) && i < object->element_count(); ++i) {
    ObString key;
    ObJsonNode *value = nullptr;
    int64_t idx = -1;
    if (OB_FAIL(static_cast<ObJsonObject *>(object)->get_value_by_idx(i, key, value))) {
    } else if (OB_ISNULL(value)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (-1 == (idx = find_column(columns_, key, static_cast<int64_t>(i)))) {
      ret = ObFileSqlUtil::report_error(options_.path_, json_->line_no(), "unknown key '%.*s'",
                                        key.length(), key.ptr());
    } else if (!needed_.at(idx)) {
    } else if (ObJsonNodeType::J_OBJECT == value->json_type() || ObJsonNodeType::J_ARRAY == value->json_type()) {
      ret = ObFileSqlUtil::report_error(options_.path_, json_->line_no(), "nested value of key '%.*s' is not supported",
                                        key.length(), key.ptr());
    } else if (OB_FAIL(convert_json(columns_.at(idx), value, json_->line_no(), values.at(idx)))) {
    }
  }
  return ret;
}

int ObFileRowReader::next_parquet_row(ObIArray<ObFileValue> &values)
{
  int ret = OB_SUCCESS;
  while (OB_SUCC(ret) && parquet_rows_left_ <= 0) {
    ++parquet_row_group_;
    if (parquet_row_group_ >= parquet_->row_group_count()) {
      ret = OB_ITER_END;
    } else {
      parquet_rows_left_ = parquet_->row_group_rows(parquet_row_group_);
      for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
        ObParquetColumnReader *&reader = parquet_columns_.at(i);
        if (!needed_.at(i)) {
        } else if (OB_ISNULL(reader)) {
          void *buf = allocator_.alloc(sizeof(ObParquetColumnReader));
          if (OB_ISNULL(buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            reader = new (buf) ObParquetColumnReader();
          }
        }
        if (OB_SUCC(ret) && OB_NOT_NULL(reader) && needed_.at(i)
            && OB_FAIL(reader->open(parquet_, &file_, parquet_row_group_, columns_.at(i).src_idx_))) {
        }
      }
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns_.count(); ++i) {
    if (needed_.at(i) && OB_FAIL(parquet_columns_.at(i)->next_value(row_allocator_, values.at(i)))) {
      if (OB_ITER_END == ret) {
        ret = ObFileSqlUtil::report_error(options_.path_, 0, "column '%.*s' has fewer values than its row group",
                                          columns_.at(i).name_.length(), columns_.at(i).name_.ptr());
      }
    }
  }
  if (OB_SUCC(ret)) {
    --parquet_rows_left_;
  }
  return ret;
}

int ObFileRowReader::next_glob_row(ObIArray<ObFileValue> &values)
{
  int ret = OB_SUCCESS;
  if (glob_pos_ >= glob_files_.count()) {
    ret = OB_ITER_END;
  } else {
    values.at(0).is_null_ = false;
    values.at(0).str_ = glob_files_.at(glob_pos_++);
  }
  return ret;
}

int ob_file_sql_glob(ObIAllocator &allocator, const ObString &pattern, ObIArray<ObString> &files)
{
  int ret = OB_SUCCESS;
  ObSqlString pattern_str;
  glob_t result;
  MEMSET(&result, 0, sizeof(result));
  files.reuse();
  if (OB_FAIL(pattern_str.append(pattern))) {
  } else {
    const int glob_ret = ::glob(pattern_str.ptr(), 0, nullptr, &result);
    if (GLOB_NOMATCH == glob_ret) {
    } else if (0 != glob_ret) {
      ret = ObFileSqlUtil::report_error(pattern, 0, "glob failed");
    } else if (static_cast<int64_t>(result.gl_pathc) > ObFileSqlLimits::MAX_GLOB_FILES) {
      ret = ObFileSqlUtil::report_error(pattern, 0, "more than %ld files match", ObFileSqlLimits::MAX_GLOB_FILES);
    }
    for (size_t i = 0; OB_SUCC(ret) && i < result.gl_pathc; ++i) {
      struct stat st;
      ObString file;
      if (0 != ::stat(result.gl_pathv[i], &st) || !S_ISREG(st.st_mode)) {
      } else if (OB_FAIL(ob_write_string(allocator, ObString::make_string(result.gl_pathv[i]), file))) {
      } else if (OB_FAIL(files.push_back(file))) {
      }
    }
    if (0 == glob_ret || GLOB_NOMATCH == glob_ret) {
      ::globfree(&result);
    }
  }
  return ret;
}

int ObSnappyDecoder::get_uncompressed_length(const char *src, const int64_t src_len, int64_t &length)
{
  int ret = OB_SUCCESS;
  uint64_t value = 0;
  int64_t shift = 0;
  int64_t i = 0;
  bool done = false;
  for (; !done && i < src_len && shift <= 28; ++i, shift += 7) {
    const uint8_t b = static_cast<uint8_t>(src[i]);
    value |= static_cast<uint64_t>(b & 0x7F) << shift;
    done = (0 == (b & 0x80));
  }
  if (!done || value > static_cast<uint64_t>(INT32_MAX)) {
    ret = OB_INVALID_DATA;
  } else {
    length = static_cast<int64_t>(value);
  }
  return ret;
}

int ObSnappyDecoder::decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len)
{
  int ret = OB_SUCCESS;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(src);
  const uint8_t *end = p + src_len;
  int64_t length = 0;
  int64_t out = 0;
  // skip the length preamble
  while (p < end && (*p & 0x80)) {
    ++p;
  }
  ++p;
  if (OB_FAIL(get_uncompressed_length(src, src_len, length))) {
  } else if (length != dst_len) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && p < end) {
    const uint8_t tag = *p++;
    int64_t len = 0;
    int64_t offset = 0;
    switch (tag & 3) {
      case 0: {   // literal
        len = tag >> 2;
        if (len >= 60) {
          const int64_t bytes = len - 59;
          if (end - p < bytes) {
            ret = OB_INVALID_DATA;
          } else {
            len = 0;
            for (int64_t i = 0; i < bytes; ++i) {
              len |= static_cast<int64_t>(p[i]) << (8 * i);
            }
            p += bytes;
          }
        }
        ++len;
        if (OB_FAIL(ret)) {
        } else if (end - p < len || dst_len - out < len) {
          ret = OB_INVALID_DATA;
        } else {
          MEMCPY(dst + out, p, len);
          p += len;
          out += len;
        }
        break;
      }
      case 1:
        if (end - p < 1) {
          ret = OB_INVALID_DATA;
        } else {
          len = ((tag >> 2) & 7) + 4;
          offset = (static_cast<int64_t>(tag >> 5) << 8) | *p++;
        }
        break;
      case 2:
        if (end - p < 2) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = p[0] | (static_cast<int64_t>(p[1]) << 8);
          p += 2;
        }
        break;
      case 3:
        if (end - p < 4) {
          ret = OB_INVALID_DATA;
        } else {
          len = (tag >> 2) + 1;
          offset = p[0] | (static_cast<int64_t>(p[1]) << 8) | (static_cast<int64_t>(p[2]) << 16)
                   | (static_cast<int64_t>(p[3]) << 24);
          p += 4;
        }
        break;
    }
    if (OB_SUCC(ret) && 0 != (tag & 3)) {
      if (offset <= 0 || offset > out || dst_len - out < len) {
        ret = OB_INVALID_DATA;
      } else {
        // the source may overlap the destination
        for (int64_t i = 0; i < len; ++i) {
          dst[out + i] = dst[out - offset + i];
        }
        out += len;
      }
    }
  }
  if (OB_SUCC(ret) && out != dst_len) {
    ret = OB_INVALID_DATA;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
