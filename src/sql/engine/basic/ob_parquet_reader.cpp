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
#include "sql/engine/basic/ob_parquet_reader.h"
#include "lib/allocator/ob_malloc.h"
#include "lib/compress/zstd_1_3_8/ob_zstd_wrapper.h"
#include <math.h>
#include <zlib.h>

namespace oceanbase
{
using namespace common;
namespace sql
{

namespace
{
const int64_t USECS_PER_DAY = 86400L * 1000000;
const int64_t JULIAN_DAY_OF_EPOCH = 2440588;
const int64_t MAX_THRIFT_DEPTH = 64;
const int64_t MAX_PAGE_VALUES = 16L * 1024 * 1024;

// parquet.thrift enums
enum
{
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3,
};
enum
{
  ENC_PLAIN = 0,
  ENC_PLAIN_DICTIONARY = 2,
  ENC_RLE = 3,
  ENC_BIT_PACKED = 4,
  ENC_DELTA_BINARY_PACKED = 5,
  ENC_DELTA_LENGTH_BYTE_ARRAY = 6,
  ENC_DELTA_BYTE_ARRAY = 7,
  ENC_RLE_DICTIONARY = 8,
  ENC_BYTE_STREAM_SPLIT = 9,
};
enum
{
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY = 1,
  CODEC_GZIP = 2,
  CODEC_ZSTD = 6,
};
enum
{
  CT_UTF8 = 0,
  CT_DECIMAL = 5,
  CT_DATE = 6,
  CT_TIME_MILLIS = 7,
  CT_TIME_MICROS = 8,
  CT_TIMESTAMP_MILLIS = 9,
  CT_TIMESTAMP_MICROS = 10,
  CT_UINT_8 = 11,
  CT_UINT_16 = 12,
  CT_UINT_32 = 13,
  CT_UINT_64 = 14,
};
enum
{
  LT_STRING = 1,
  LT_DECIMAL = 5,
  LT_DATE = 6,
  LT_TIME = 7,
  LT_TIMESTAMP = 8,
  LT_INTEGER = 10,
  LT_UUID = 14,
  LT_FLOAT16 = 15,
};
enum
{
  REP_REQUIRED = 0,
  REP_OPTIONAL = 1,
  REP_REPEATED = 2,
};

// Thrift compact protocol
enum
{
  TT_STOP = 0,
  TT_TRUE = 1,
  TT_FALSE = 2,
  TT_BYTE = 3,
  TT_I16 = 4,
  TT_I32 = 5,
  TT_I64 = 6,
  TT_DOUBLE = 7,
  TT_BINARY = 8,
  TT_LIST = 9,
  TT_SET = 10,
  TT_MAP = 11,
  TT_STRUCT = 12,
};

class ThriftReader
{
public:
  ThriftReader(const char *p, const int64_t len)
    : p_(reinterpret_cast<const uint8_t *>(p)), begin_(p_), end_(p_ + len), ok_(true), depth_(0) {}
  bool ok() const { return ok_; }
  int64_t consumed() const { return p_ - begin_; }
  uint64_t varint()
  {
    uint64_t value = 0;
    int shift = 0;
    while (ok_) {
      if (p_ >= end_ || shift > 63) {
        ok_ = false;
      } else {
        const uint8_t b = *p_++;
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        shift += 7;
        if (0 == (b & 0x80)) {
          break;
        }
      }
    }
    return value;
  }
  int64_t zigzag()
  {
    const uint64_t v = varint();
    return static_cast<int64_t>(v >> 1) ^ -static_cast<int64_t>(v & 1);
  }
  ObString binary()
  {
    const uint64_t len = varint();
    ObString value;
    if (!ok_ || static_cast<uint64_t>(end_ - p_) < len) {
      ok_ = false;
    } else {
      value.assign_ptr(reinterpret_cast<const char *>(p_), static_cast<int32_t>(len));
      p_ += len;
    }
    return value;
  }
  uint8_t byte()
  {
    uint8_t b = 0;
    if (p_ >= end_) {
      ok_ = false;
    } else {
      b = *p_++;
    }
    return b;
  }
  // false at the end of the struct
  bool next_field(int16_t &last_id, int16_t &id, uint8_t &type)
  {
    bool has_field = false;
    const uint8_t b = byte();
    if (ok_ && TT_STOP != (b & 0x0F)) {
      type = b & 0x0F;
      const int16_t delta = b >> 4;
      id = 0 == delta ? static_cast<int16_t>(zigzag()) : static_cast<int16_t>(last_id + delta);
      last_id = id;
      has_field = ok_;
    }
    return has_field;
  }
  void list_header(uint8_t &elem_type, int64_t &size)
  {
    const uint8_t b = byte();
    elem_type = b & 0x0F;
    size = b >> 4;
    if (15 == size) {
      size = static_cast<int64_t>(varint());
    }
    if (size < 0 || size > end_ - p_) {
      ok_ = false;
    }
  }
  bool field_bool(const uint8_t type) { return TT_TRUE == type; }
  int32_t i32() { return static_cast<int32_t>(zigzag()); }
  void skip(const uint8_t type)
  {
    if (++depth_ > MAX_THRIFT_DEPTH) {
      ok_ = false;
    }
    switch (type) {
      case TT_TRUE:
      case TT_FALSE:
        break;
      case TT_BYTE:
        byte();
        break;
      case TT_I16:
      case TT_I32:
      case TT_I64:
        varint();
        break;
      case TT_DOUBLE:
        if (end_ - p_ < 8) {
          ok_ = false;
        } else {
          p_ += 8;
        }
        break;
      case TT_BINARY:
        binary();
        break;
      case TT_LIST:
      case TT_SET: {
        uint8_t elem_type = 0;
        int64_t size = 0;
        list_header(elem_type, size);
        for (int64_t i = 0; ok_ && i < size; ++i) {
          if (TT_TRUE == elem_type || TT_FALSE == elem_type) {
            byte();
          } else {
            skip(elem_type);
          }
        }
        break;
      }
      case TT_MAP: {
        const int64_t size = static_cast<int64_t>(varint());
        if (size > 0) {
          const uint8_t kv = byte();
          for (int64_t i = 0; ok_ && i < size; ++i) {
            skip(kv >> 4);
            skip(kv & 0x0F);
          }
        }
        break;
      }
      case TT_STRUCT: {
        int16_t last_id = 0;
        int16_t id = 0;
        uint8_t field_type = 0;
        while (ok_ && next_field(last_id, id, field_type)) {
          skip(field_type);
        }
        break;
      }
      default:
        ok_ = false;
        break;
    }
    --depth_;
  }

private:
  const uint8_t *p_;
  const uint8_t *begin_;
  const uint8_t *end_;
  bool ok_;
  int64_t depth_;
};

struct SchemaElement
{
  SchemaElement()
    : type_(-1), type_length_(0), repetition_(REP_REQUIRED), name_(), num_children_(0),
      converted_type_(-1), scale_(0), logical_type_(0), logical_unit_(0), integer_signed_(true) {}
  int32_t type_;
  int32_t type_length_;
  int32_t repetition_;
  ObString name_;
  int32_t num_children_;
  int32_t converted_type_;
  int32_t scale_;
  int32_t logical_type_;
  int64_t logical_unit_;     // values per second of TIME and TIMESTAMP
  bool integer_signed_;
};

int64_t read_time_unit(ThriftReader &r)
{
  int64_t unit = 1000000;
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  while (r.ok() && r.next_field(last_id, id, type)) {
    unit = 1 == id ? 1000 : (2 == id ? 1000000 : 1000000000);
    r.skip(type);
  }
  return unit;
}

void read_logical_type(ThriftReader &r, SchemaElement &element)
{
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  while (r.ok() && r.next_field(last_id, id, type)) {
    element.logical_type_ = id;
    if (TT_STRUCT != type) {
      r.skip(type);
    } else {
      int16_t inner_last = 0;
      int16_t inner_id = 0;
      uint8_t inner_type = 0;
      while (r.ok() && r.next_field(inner_last, inner_id, inner_type)) {
        if (LT_DECIMAL == id && 1 == inner_id) {
          element.scale_ = r.i32();
        } else if ((LT_TIME == id || LT_TIMESTAMP == id) && 2 == inner_id && TT_STRUCT == inner_type) {
          element.logical_unit_ = read_time_unit(r);
        } else if (LT_INTEGER == id && 2 == inner_id) {
          element.integer_signed_ = r.field_bool(inner_type);
          r.skip(inner_type);
        } else {
          r.skip(inner_type);
        }
      }
    }
  }
}

void read_schema_element(ThriftReader &r, SchemaElement &element)
{
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  while (r.ok() && r.next_field(last_id, id, type)) {
    switch (id) {
      case 1: element.type_ = r.i32(); break;
      case 2: element.type_length_ = r.i32(); break;
      case 3: element.repetition_ = r.i32(); break;
      case 4: element.name_ = r.binary(); break;
      case 5: element.num_children_ = r.i32(); break;
      case 6: element.converted_type_ = r.i32(); break;
      case 7: if (0 == element.scale_) { element.scale_ = r.i32(); } else { r.skip(type); } break;
      case 10:
        if (TT_STRUCT == type) {
          read_logical_type(r, element);
        } else {
          r.skip(type);
        }
        break;
      default: r.skip(type); break;
    }
  }
}

void read_column_chunk(ThriftReader &r, ObParquetChunk &chunk, bool &external)
{
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  int64_t data_offset = -1;
  int64_t dict_offset = -1;
  while (r.ok() && r.next_field(last_id, id, type)) {
    if (1 == id && TT_BINARY == type) {
      external = !r.binary().empty();
    } else if (3 == id && TT_STRUCT == type) {
      int16_t meta_last = 0;
      int16_t meta_id = 0;
      uint8_t meta_type = 0;
      while (r.ok() && r.next_field(meta_last, meta_id, meta_type)) {
        switch (meta_id) {
          case 1: chunk.physical_type_ = r.i32(); break;
          case 4: chunk.codec_ = r.i32(); break;
          case 5: chunk.num_values_ = r.zigzag(); break;
          case 7: chunk.total_size_ = r.zigzag(); break;
          case 9: data_offset = r.zigzag(); break;
          case 11: dict_offset = r.zigzag(); break;
          default: r.skip(meta_type); break;
        }
      }
    } else {
      r.skip(type);
    }
  }
  chunk.start_offset_ = (dict_offset > 0 && dict_offset < data_offset) ? dict_offset : data_offset;
}

struct PageHeader
{
  PageHeader()
    : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0), num_nulls_(0),
      encoding_(ENC_PLAIN), def_encoding_(ENC_RLE), def_length_(0), rep_length_(0), is_compressed_(true) {}
  int32_t type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t num_nulls_;
  int32_t encoding_;
  int32_t def_encoding_;
  int32_t def_length_;
  int32_t rep_length_;
  bool is_compressed_;
};

void read_page_header(ThriftReader &r, PageHeader &header)
{
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  while (r.ok() && r.next_field(last_id, id, type)) {
    if (1 == id) {
      header.type_ = r.i32();
    } else if (2 == id) {
      header.uncompressed_size_ = r.i32();
    } else if (3 == id) {
      header.compressed_size_ = r.i32();
    } else if ((5 == id || 7 == id || 8 == id) && TT_STRUCT == type) {
      // data page, dictionary page or data page v2
      int16_t inner_last = 0;
      int16_t inner_id = 0;
      uint8_t inner_type = 0;
      while (r.ok() && r.next_field(inner_last, inner_id, inner_type)) {
        if (1 == inner_id) {
          header.num_values_ = r.i32();
        } else if (5 == id && 2 == inner_id) {
          header.encoding_ = r.i32();
        } else if (5 == id && 3 == inner_id) {
          header.def_encoding_ = r.i32();
        } else if (7 == id && 2 == inner_id) {
          header.encoding_ = r.i32();
        } else if (8 == id && 2 == inner_id) {
          header.num_nulls_ = r.i32();
        } else if (8 == id && 4 == inner_id) {
          header.encoding_ = r.i32();
        } else if (8 == id && 5 == inner_id) {
          header.def_length_ = r.i32();
        } else if (8 == id && 6 == inner_id) {
          header.rep_length_ = r.i32();
        } else if (8 == id && 7 == inner_id) {
          header.is_compressed_ = r.field_bool(inner_type);
          r.skip(inner_type);
        } else {
          r.skip(inner_type);
        }
      }
    } else {
      r.skip(type);
    }
  }
}

inline uint64_t read_le(const uint8_t *p, const int64_t bytes)
{
  uint64_t v = 0;
  for (int64_t i = 0; i < bytes; ++i) {
    v |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  return v;
}

// bit_width bits at bit offset, least significant bit first
inline uint64_t read_bits(const uint8_t *data, const int64_t bit_offset, const int64_t bit_width)
{
  uint64_t v = 0;
  int64_t done = 0;
  int64_t byte = bit_offset >> 3;
  int64_t shift = bit_offset & 7;
  while (done < bit_width) {
    const int64_t take = MIN(8 - shift, bit_width - done);
    const uint64_t bits = (static_cast<uint64_t>(data[byte]) >> shift) & ((1ULL << take) - 1);
    v |= bits << done;
    done += take;
    shift = 0;
    ++byte;
  }
  return v;
}

bool read_uvarint(const uint8_t *&p, const uint8_t *end, uint64_t &value)
{
  value = 0;
  int shift = 0;
  bool ok = false;
  while (p < end && shift <= 63) {
    const uint8_t b = *p++;
    value |= static_cast<uint64_t>(b & 0x7F) << shift;
    shift += 7;
    if (0 == (b & 0x80)) {
      ok = true;
      break;
    }
  }
  return ok;
}

// RLE / bit packed hybrid
template <typename T>
bool decode_rle(const uint8_t *p, const uint8_t *end, const int64_t bit_width, const int64_t count,
                ObArray<T> &out)
{
  bool ok = bit_width >= 0 && bit_width <= 32;
  out.reuse();
  if (ok && OB_SUCCESS != out.reserve(count)) {
    ok = false;
  }
  while (ok && out.count() < count) {
    uint64_t header = 0;
    if (!read_uvarint(p, end, header)) {
      ok = false;
    } else if (header & 1) {
      const int64_t values = static_cast<int64_t>(header >> 1) * 8;
      const int64_t bytes = static_cast<int64_t>(header >> 1) * bit_width;
      if (end - p < bytes) {
        ok = false;
      }
      for (int64_t i = 0; ok && i < values && out.count() < count; ++i) {
        ok = OB_SUCCESS == out.push_back(static_cast<T>(read_bits(p, i * bit_width, bit_width)));
      }
      p += bytes;
    } else {
      const int64_t run = static_cast<int64_t>(header >> 1);
      const int64_t bytes = (bit_width + 7) / 8;
      if (end - p < bytes || run <= 0) {
        ok = false;
      } else {
        const T value = static_cast<T>(read_le(p, bytes));
        p += bytes;
        for (int64_t i = 0; ok && i < run && out.count() < count; ++i) {
          ok = OB_SUCCESS == out.push_back(value);
        }
      }
    }
  }
  return ok;
}

bool decode_delta_binary_packed(const uint8_t *&p, const uint8_t *end, ObArray<int64_t> &out)
{
  uint64_t block_size = 0;
  uint64_t miniblocks = 0;
  uint64_t total = 0;
  uint64_t first = 0;
  bool ok = read_uvarint(p, end, block_size) && read_uvarint(p, end, miniblocks)
            && read_uvarint(p, end, total) && read_uvarint(p, end, first)
            && miniblocks > 0 && block_size > 0 && block_size % miniblocks == 0
            && (block_size / miniblocks) % 8 == 0 && total <= static_cast<uint64_t>(INT32_MAX);
  const int64_t values_per_mini = ok ? static_cast<int64_t>(block_size / miniblocks) : 0;
  uint64_t last = (first >> 1) ^ -(first & 1);
  out.reuse();
  if (ok && total > 0) {
    ok = OB_SUCCESS == out.reserve(total) && OB_SUCCESS == out.push_back(static_cast<int64_t>(last));
  }
  while (ok && static_cast<uint64_t>(out.count()) < total) {
    uint64_t zz = 0;
    if (!read_uvarint(p, end, zz) || end - p < static_cast<int64_t>(miniblocks)) {
      ok = false;
    } else {
      const uint64_t min_delta = (zz >> 1) ^ -(zz & 1);
      const uint8_t *widths = p;
      p += miniblocks;
      for (uint64_t m = 0; ok && m < miniblocks && static_cast<uint64_t>(out.count()) < total; ++m) {
        const int64_t width = widths[m];
        const int64_t bytes = values_per_mini * width / 8;
        if (width > 64 || end - p < bytes) {
          ok = false;
        }
        for (int64_t i = 0; ok && i < values_per_mini && static_cast<uint64_t>(out.count()) < total; ++i) {
          last += min_delta + read_bits(p, i * width, width);
          ok = OB_SUCCESS == out.push_back(static_cast<int64_t>(last));
        }
        p += bytes;
      }
    }
  }
  return ok;
}

void *zstd_alloc(void *opaque, size_t size)
{
  UNUSED(opaque);
  return ob_malloc(size, ObMemAttr("FileSqlZstd"));
}

void zstd_free(void *opaque, void *address)
{
  UNUSED(opaque);
  ob_free(address);
}

void *page_malloc(const int64_t size)
{
  return ob_malloc(MAX(size, 1), ObMemAttr("FileSqlPage"));
}

double half_to_double(const uint16_t h)
{
  const int sign = (h >> 15) & 1;
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  double value = 0;
  if (0 == exponent) {
    value = ldexp(mantissa, -24);
  } else if (31 == exponent) {
    value = 0 == mantissa ? INFINITY : NAN;
  } else {
    value = ldexp(mantissa + 1024, exponent - 25);
  }
  return sign ? -value : value;
}
} // namespace

void ObParquetFile::reset()
{
  for (int64_t i = 0; i < row_groups_.count(); ++i) {
    if (OB_NOT_NULL(row_groups_.at(i))) {
      row_groups_.at(i)->~ObParquetRowGroup();
    }
  }
  row_groups_.reset();
  leaves_.reset();
  file_ = nullptr;
}

int ObParquetFile::open(ObFileReader *file)
{
  int ret = OB_SUCCESS;
  char tail[8];
  char head[4];
  reset();
  file_ = file;
  if (OB_ISNULL(file)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (file->file_size() < 12) {
    ret = ObFileSqlUtil::report_error(file->path(), 0, "not a Parquet file");
  } else if (OB_FAIL(file->pread(0, 4, head)) || OB_FAIL(file->pread(file->file_size() - 8, 8, tail))) {
  } else if (0 != MEMCMP(head, "PAR1", 4) || 0 != MEMCMP(tail + 4, "PAR1", 4)) {
    ret = ObFileSqlUtil::report_error(file->path(), 0, "not a Parquet file");
  } else {
    const int64_t footer_len = static_cast<int64_t>(read_le(reinterpret_cast<const uint8_t *>(tail), 4));
    char *footer = nullptr;
    if (footer_len <= 0 || footer_len > file->file_size() - 12 || footer_len > ObFileSqlLimits::MAX_FOOTER_SIZE) {
      ret = ObFileSqlUtil::report_error(file->path(), 0, "invalid Parquet footer length %ld", footer_len);
    } else if (OB_ISNULL(footer = static_cast<char *>(allocator_.alloc(footer_len)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_FAIL(file->pread(file->file_size() - 8 - footer_len, footer_len, footer))) {
    } else if (OB_FAIL(parse_footer(footer, footer_len))) {
    }
  }
  return ret;
}

int ObParquetFile::parse_footer(const char *buf, const int64_t len)
{
  int ret = OB_SUCCESS;
  ThriftReader r(buf, len);
  ObSEArray<SchemaElement, 16> schema;
  int16_t last_id = 0;
  int16_t id = 0;
  uint8_t type = 0;
  bool external = false;
  while (OB_SUCC(ret) && r.ok() && r.next_field(last_id, id, type)) {
    if (2 == id && TT_LIST == type) {
      uint8_t elem_type = 0;
      int64_t size = 0;
      r.list_header(elem_type, size);
      for (int64_t i = 0; OB_SUCC(ret) && r.ok() && i < size; ++i) {
        SchemaElement element;
        read_schema_element(r, element);
        ret = schema.push_back(element);
      }
    } else if (3 == id) {
      num_rows_ = r.zigzag();
    } else if (4 == id && TT_LIST == type) {
      uint8_t elem_type = 0;
      int64_t size = 0;
      r.list_header(elem_type, size);
      for (int64_t i = 0; OB_SUCC(ret) && r.ok() && i < size; ++i) {
        void *rg_buf = allocator_.alloc(sizeof(ObParquetRowGroup));
        ObParquetRowGroup *row_group = nullptr;
        if (OB_ISNULL(rg_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (FALSE_IT(row_group = new (rg_buf) ObParquetRowGroup())) {
        } else if (FALSE_IT(row_group->num_rows_ = 0)) {
        } else if (OB_FAIL(row_groups_.push_back(row_group))) {
        }
        int16_t rg_last = 0;
        int16_t rg_id = 0;
        uint8_t rg_type = 0;
        while (OB_SUCC(ret) && r.ok() && r.next_field(rg_last, rg_id, rg_type)) {
          if (1 == rg_id && TT_LIST == rg_type) {
            uint8_t chunk_type = 0;
            int64_t chunk_count = 0;
            r.list_header(chunk_type, chunk_count);
            for (int64_t j = 0; OB_SUCC(ret) && r.ok() && j < chunk_count; ++j) {
              ObParquetChunk chunk;
              read_column_chunk(r, chunk, external);
              ret = row_group->chunks_.push_back(chunk);
            }
          } else if (3 == rg_id) {
            row_group->num_rows_ = r.zigzag();
          } else {
            r.skip(rg_type);
          }
        }
      }
    } else {
      r.skip(type);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (!r.ok() || schema.count() < 2) {
    ret = ObFileSqlUtil::report_error(path(), 0, "invalid Parquet footer");
  } else if (external) {
    ret = ObFileSqlUtil::report_error(path(), 0, "column chunks in other files are not supported");
  } else if (schema.at(0).num_children_ != schema.count() - 1) {
    ret = ObFileSqlUtil::report_error(path(), 0, "nested Parquet columns are not supported");
  }
  for (int64_t i = 1; OB_SUCC(ret) && i < schema.count(); ++i) {
    const SchemaElement &element = schema.at(i);
    ObParquetLeaf leaf;
    leaf.name_ = element.name_;
    leaf.physical_type_ = element.type_;
    leaf.type_length_ = element.type_length_;
    leaf.nullable_ = REP_OPTIONAL == element.repetition_;
    leaf.scale_ = element.scale_;
    const bool is_decimal = CT_DECIMAL == element.converted_type_ || LT_DECIMAL == element.logical_type_;
    if (element.num_children_ > 0 || element.type_ < 0 || REP_REPEATED == element.repetition_) {
      ret = ObFileSqlUtil::report_error(path(), 0, "nested or repeated column '%.*s' is not supported",
                                        element.name_.length(), element.name_.ptr());
    } else {
      switch (element.type_) {
        case PQ_BOOLEAN:
          leaf.kind_ = PQ_KIND_BOOL;
          leaf.file_type_ = ObFileColType::BOOLEAN;
          break;
        case PQ_INT32:
          if (CT_DATE == element.converted_type_ || LT_DATE == element.logical_type_) {
            leaf.kind_ = PQ_KIND_DATE;
            leaf.file_type_ = ObFileColType::DATE;
          } else if (is_decimal) {
            leaf.kind_ = PQ_KIND_DECIMAL;
            leaf.file_type_ = ObFileColType::DOUBLE;
          } else if (CT_TIME_MILLIS == element.converted_type_ || LT_TIME == element.logical_type_) {
            leaf.kind_ = PQ_KIND_TIME;
            leaf.time_unit_ = 1000;
            leaf.file_type_ = ObFileColType::VARCHAR;
            leaf.max_length_ = 15;
          } else {
            leaf.kind_ = PQ_KIND_INT;
            leaf.unsigned_ = (element.converted_type_ >= CT_UINT_8 && element.converted_type_ <= CT_UINT_32)
                             || (LT_INTEGER == element.logical_type_ && !element.integer_signed_);
            leaf.file_type_ = ObFileColType::BIGINT;
          }
          break;
        case PQ_INT64:
          if (CT_TIMESTAMP_MILLIS == element.converted_type_ || CT_TIMESTAMP_MICROS == element.converted_type_
              || LT_TIMESTAMP == element.logical_type_) {
            leaf.kind_ = PQ_KIND_TIMESTAMP;
            leaf.time_unit_ = LT_TIMESTAMP == element.logical_type_ ? element.logical_unit_
                              : (CT_TIMESTAMP_MILLIS == element.converted_type_ ? 1000 : 1000000);
            leaf.file_type_ = ObFileColType::DATETIME;
          } else if (is_decimal) {
            leaf.kind_ = PQ_KIND_DECIMAL;
            leaf.file_type_ = ObFileColType::DOUBLE;
          } else if (CT_TIME_MICROS == element.converted_type_ || LT_TIME == element.logical_type_) {
            leaf.kind_ = PQ_KIND_TIME;
            leaf.time_unit_ = LT_TIME == element.logical_type_ ? element.logical_unit_ : 1000000;
            leaf.file_type_ = ObFileColType::VARCHAR;
            leaf.max_length_ = 18;
          } else {
            leaf.kind_ = PQ_KIND_INT;
            leaf.unsigned_ = CT_UINT_64 == element.converted_type_
                             || (LT_INTEGER == element.logical_type_ && !element.integer_signed_);
            leaf.file_type_ = ObFileColType::BIGINT;
          }
          break;
        case PQ_INT96:
          leaf.kind_ = PQ_KIND_TIMESTAMP;
          leaf.time_unit_ = 1000000;
          leaf.file_type_ = ObFileColType::DATETIME;
          break;
        case PQ_FLOAT:
        case PQ_DOUBLE:
          leaf.kind_ = PQ_KIND_DOUBLE;
          leaf.file_type_ = ObFileColType::DOUBLE;
          break;
        case PQ_BYTE_ARRAY:
        case PQ_FIXED_LEN_BYTE_ARRAY:
          if (is_decimal) {
            leaf.kind_ = PQ_KIND_DECIMAL;
            leaf.file_type_ = ObFileColType::DOUBLE;
          } else if (PQ_FIXED_LEN_BYTE_ARRAY == element.type_ && LT_UUID == element.logical_type_
                     && 16 == element.type_length_) {
            leaf.kind_ = PQ_KIND_UUID;
            leaf.file_type_ = ObFileColType::VARCHAR;
            leaf.max_length_ = 36;
          } else if (PQ_FIXED_LEN_BYTE_ARRAY == element.type_ && LT_FLOAT16 == element.logical_type_
                     && 2 == element.type_length_) {
            leaf.kind_ = PQ_KIND_FLOAT16;
            leaf.file_type_ = ObFileColType::DOUBLE;
          } else {
            leaf.kind_ = PQ_KIND_STRING;
            leaf.file_type_ = ObFileColType::VARCHAR;
            leaf.max_length_ = PQ_FIXED_LEN_BYTE_ARRAY == element.type_
                               ? MAX(element.type_length_, 1) : ObFileSqlLimits::MAX_VARCHAR_LENGTH;
          }
          break;
        default:
          ret = ObFileSqlUtil::report_error(path(), 0, "unknown physical type %d of column '%.*s'",
                                            element.type_, element.name_.length(), element.name_.ptr());
          break;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(leaves_.push_back(leaf))) {
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < row_groups_.count(); ++i) {
    const ObParquetRowGroup &row_group = *row_groups_.at(i);
    if (row_group.chunks_.count() != leaves_.count() || row_group.num_rows_ < 0) {
      ret = ObFileSqlUtil::report_error(path(), 0, "row group %ld does not match the schema", i);
    }
    // a row group without rows is never read
    for (int64_t j = 0; OB_SUCC(ret) && row_group.num_rows_ > 0 && j < row_group.chunks_.count(); ++j) {
      const ObParquetChunk &chunk = row_group.chunks_.at(j);
      if (chunk.start_offset_ < 4 || chunk.total_size_ < 0
          || chunk.start_offset_ + chunk.total_size_ > file_->file_size()
          || chunk.physical_type_ != leaves_.at(j).physical_type_
          || chunk.num_values_ != row_group.num_rows_) {
        ret = ObFileSqlUtil::report_error(path(), 0, "invalid column chunk %ld of row group %ld", j, i);
      }
    }
  }
  return ret;
}

ObParquetColumnReader::ObParquetColumnReader()
  : file_(nullptr), reader_(nullptr), leaf_(), chunk_(), offset_(0), values_read_(0),
    page_buf_(nullptr), values_buf_(nullptr), page_values_(0), page_pos_(0), value_pos_(0),
    page_uses_dict_(false), dict_buf_(nullptr), dict_values_buf_(nullptr), has_dict_(false)
{
}

void ObParquetColumnReader::free_page()
{
  if (OB_NOT_NULL(page_buf_)) {
    ob_free(page_buf_);
    page_buf_ = nullptr;
  }
  if (OB_NOT_NULL(values_buf_)) {
    ob_free(values_buf_);
    values_buf_ = nullptr;
  }
  page_values_ = 0;
  page_pos_ = 0;
  value_pos_ = 0;
  ints_.reuse();
  doubles_.reuse();
  strings_.reuse();
  def_levels_.reuse();
}

void ObParquetColumnReader::reset()
{
  free_page();
  if (OB_NOT_NULL(dict_buf_)) {
    ob_free(dict_buf_);
    dict_buf_ = nullptr;
  }
  if (OB_NOT_NULL(dict_values_buf_)) {
    ob_free(dict_values_buf_);
    dict_values_buf_ = nullptr;
  }
  dict_ints_.reuse();
  dict_doubles_.reuse();
  dict_strings_.reuse();
  has_dict_ = false;
}

int ObParquetColumnReader::report(const char *msg)
{
  return ObFileSqlUtil::report_error(file_->path(), 0, "column '%.*s': %s",
                                     leaf_.name_.length(), leaf_.name_.ptr(), msg);
}

int ObParquetColumnReader::open(ObParquetFile *file, ObFileReader *reader, const int64_t row_group,
                                const int64_t column)
{
  int ret = OB_SUCCESS;
  reset();
  if (OB_ISNULL(file) || OB_ISNULL(reader)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    file_ = file;
    reader_ = reader;
    leaf_ = file->column(column);
    chunk_ = file->chunk(row_group, column);
    offset_ = chunk_.start_offset_;
    values_read_ = 0;
  }
  return ret;
}

int ObParquetColumnReader::decompress(const char *src, const int64_t src_len, char *dst, const int64_t dst_len)
{
  int ret = OB_SUCCESS;
  switch (chunk_.codec_) {
    case CODEC_UNCOMPRESSED:
      if (src_len != dst_len) {
        ret = report("page size mismatch");
      } else {
        MEMCPY(dst, src, src_len);
      }
      break;
    case CODEC_SNAPPY:
      if (OB_FAIL(ObSnappyDecoder::decompress(src, src_len, dst, dst_len))) {
        ret = report("invalid SNAPPY data");
      }
      break;
    case CODEC_GZIP: {
      z_stream zs;
      MEMSET(&zs, 0, sizeof(zs));
      if (Z_OK != inflateInit2(&zs, 15 + 32)) {
        ret = report("failed to start GZIP decompression");
      } else {
        zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src));
        zs.avail_in = static_cast<uInt>(src_len);
        zs.next_out = reinterpret_cast<Bytef *>(dst);
        zs.avail_out = static_cast<uInt>(dst_len);
        const int zret = inflate(&zs, Z_FINISH);
        if ((Z_STREAM_END != zret && Z_OK != zret) || static_cast<int64_t>(zs.total_out) != dst_len) {
          ret = report("invalid GZIP data");
        }
        inflateEnd(&zs);
      }
      break;
    }
    case CODEC_ZSTD: {
      zstd_1_3_8::OB_ZSTD_customMem mem;
      mem.customAlloc = zstd_alloc;
      mem.customFree = zstd_free;
      mem.opaque = nullptr;
      size_t out_size = 0;
      if (OB_SUCCESS != zstd_1_3_8::ObZstdWrapper::decompress(mem, src, src_len, dst, dst_len, out_size)
          || static_cast<int64_t>(out_size) != dst_len) {
        ret = report("invalid ZSTD data");
      }
      break;
    }
    default:
      ret = report("unsupported compression codec");
      break;
  }
  return ret;
}

int ObParquetColumnReader::decode_values(const char *data, const int64_t len, const int32_t encoding,
                                         const int64_t count, const bool is_dictionary)
{
  int ret = OB_SUCCESS;
  ObArray<int64_t> &ints = is_dictionary ? dict_ints_ : ints_;
  ObArray<double> &doubles = is_dictionary ? dict_doubles_ : doubles_;
  ObArray<ObString> &strings = is_dictionary ? dict_strings_ : strings_;
  const uint8_t *p = reinterpret_cast<const uint8_t *>(data);
  const uint8_t *end = p + len;
  const int32_t type = leaf_.physical_type_;
  ints.reuse();
  doubles.reuse();
  strings.reuse();
  if (count < 0) {
    ret = report("invalid value count");
  } else if (ENC_PLAIN == encoding) {
    int64_t width = 0;
    switch (type) {
      case PQ_BOOLEAN:
        if ((count + 7) / 8 > len) {
          ret = report("page is too short");
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ret = ints.push_back((p[i >> 3] >> (i & 7)) & 1);
        }
        break;
      case PQ_INT32:
      case PQ_INT64:
      case PQ_INT96:
      case PQ_FLOAT:
      case PQ_DOUBLE:
        width = PQ_INT32 == type || PQ_FLOAT == type ? 4 : (PQ_INT96 == type ? 12 : 8);
        if (width * count > len) {
          ret = report("page is too short");
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          const uint8_t *v = p + i * width;
          if (PQ_INT32 == type) {
            ret = ints.push_back(static_cast<int32_t>(read_le(v, 4)));
          } else if (PQ_INT64 == type) {
            ret = ints.push_back(static_cast<int64_t>(read_le(v, 8)));
          } else if (PQ_INT96 == type) {
            const int64_t nanos = static_cast<int64_t>(read_le(v, 8));
            const int64_t julian_day = static_cast<int32_t>(read_le(v + 8, 4));
            ret = ints.push_back((julian_day - JULIAN_DAY_OF_EPOCH) * USECS_PER_DAY + nanos / 1000);
          } else if (PQ_FLOAT == type) {
            float f = 0;
            const uint32_t bits = static_cast<uint32_t>(read_le(v, 4));
            MEMCPY(&f, &bits, 4);
            ret = doubles.push_back(f);
          } else {
            double d = 0;
            const uint64_t bits = read_le(v, 8);
            MEMCPY(&d, &bits, 8);
            ret = doubles.push_back(d);
          }
        }
        break;
      case PQ_BYTE_ARRAY:
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          if (end - p < 4) {
            ret = report("page is too short");
          } else {
            const int64_t size = static_cast<int64_t>(read_le(p, 4));
            p += 4;
            if (size > end - p) {
              ret = report("page is too short");
            } else {
              ret = strings.push_back(ObString(static_cast<int32_t>(size), reinterpret_cast<const char *>(p)));
              p += size;
            }
          }
        }
        break;
      case PQ_FIXED_LEN_BYTE_ARRAY:
        width = leaf_.type_length_;
        if (width <= 0 || width * count > len) {
          ret = report("page is too short");
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
          ret = strings.push_back(ObString(static_cast<int32_t>(width), data + i * width));
        }
        break;
      default:
        ret = report("unknown physical type");
        break;
    }
  } else if (ENC_RLE == encoding && PQ_BOOLEAN == type) {
    if (len < 4 || static_cast<int64_t>(read_le(p, 4)) > len - 4
        || !decode_rle(p + 4, p + 4 + read_le(p, 4), 1, count, ints)) {
      ret = report("invalid RLE boolean data");
    }
  } else if (ENC_DELTA_BINARY_PACKED == encoding && (PQ_INT32 == type || PQ_INT64 == type)) {
    if (!decode_delta_binary_packed(p, end, ints) || ints.count() < count) {
      ret = report("invalid DELTA_BINARY_PACKED data");
    }
    for (int64_t i = 0; OB_SUCC(ret) && PQ_INT32 == type && i < ints.count(); ++i) {
      ints.at(i) = static_cast<int32_t>(ints.at(i));
    }
  } else if (ENC_DELTA_LENGTH_BYTE_ARRAY == encoding && PQ_BYTE_ARRAY == type) {
    ObArray<int64_t> lengths;
    if (!decode_delta_binary_packed(p, end, lengths) || lengths.count() < count) {
      ret = report("invalid DELTA_LENGTH_BYTE_ARRAY data");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      if (lengths.at(i) < 0 || lengths.at(i) > end - p) {
        ret = report("invalid DELTA_LENGTH_BYTE_ARRAY data");
      } else {
        ret = strings.push_back(ObString(static_cast<int32_t>(lengths.at(i)), reinterpret_cast<const char *>(p)));
        p += lengths.at(i);
      }
    }
  } else if (ENC_DELTA_BYTE_ARRAY == encoding
             && (PQ_BYTE_ARRAY == type || PQ_FIXED_LEN_BYTE_ARRAY == type)) {
    ObArray<int64_t> prefixes;
    ObArray<int64_t> suffixes;
    int64_t total = 0;
    char *&buf = is_dictionary ? dict_values_buf_ : values_buf_;
    if (!decode_delta_binary_packed(p, end, prefixes) || !decode_delta_binary_packed(p, end, suffixes)
        || prefixes.count() < count || suffixes.count() < count) {
      ret = report("invalid DELTA_BYTE_ARRAY data");
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      if (prefixes.at(i) < 0 || suffixes.at(i) < 0 || suffixes.at(i) > len) {
        ret = report("invalid DELTA_BYTE_ARRAY data");
      } else {
        total += prefixes.at(i) + suffixes.at(i);
      }
    }
    if (OB_FAIL(ret)) {
    } else if (total > ObFileSqlLimits::MAX_PAGE_SIZE) {
      ret = report("page is too large");
    } else if (OB_ISNULL(buf = static_cast<char *>(page_malloc(total)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    int64_t pos = 0;
    ObString prev;
    for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
      const int64_t prefix = prefixes.at(i);
      const int64_t suffix = suffixes.at(i);
      if (prefix > prev.length() || suffix > end - p) {
        ret = report("invalid DELTA_BYTE_ARRAY data");
      } else {
        MEMCPY(buf + pos, prev.ptr(), prefix);
        MEMCPY(buf + pos + prefix, p, suffix);
        p += suffix;
        prev.assign_ptr(buf + pos, static_cast<int32_t>(prefix + suffix));
        pos += prefix + suffix;
        ret = strings.push_back(prev);
      }
    }
  } else if (ENC_BYTE_STREAM_SPLIT == encoding && PQ_BOOLEAN != type && PQ_BYTE_ARRAY != type
             && PQ_INT96 != type) {
    const int64_t width = PQ_INT32 == type || PQ_FLOAT == type ? 4
                          : (PQ_FIXED_LEN_BYTE_ARRAY == type ? leaf_.type_length_ : 8);
    char *&buf = is_dictionary ? dict_values_buf_ : values_buf_;
    if (width <= 0 || width * count > len) {
      ret = report("invalid BYTE_STREAM_SPLIT data");
    } else if (OB_ISNULL(buf = static_cast<char *>(page_malloc(width * count)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      for (int64_t i = 0; i < count; ++i) {
        for (int64_t j = 0; j < width; ++j) {
          buf[i * width + j] = data[j * count + i];
        }
      }
      ret = decode_values(buf, width * count, ENC_PLAIN, count, is_dictionary);
    }
  } else {
    ret = report("unsupported encoding");
  }
  return ret;
}

int ObParquetColumnReader::load_page()
{
  int ret = OB_SUCCESS;
  bool loaded = false;
  const int64_t chunk_end = chunk_.start_offset_ + chunk_.total_size_;
  free_page();
  while (OB_SUCC(ret) && !loaded) {
    PageHeader header;
    int64_t header_len = 0;
    int64_t window = MIN(64L * 1024, chunk_end - offset_);
    char *header_buf = nullptr;
    bool parsed = false;
    if (window <= 0) {
      ret = report("column chunk ends before all values were read");
    }
    // a page header holds statistics of unknown size, read more until it parses
    while (OB_SUCC(ret) && !parsed) {
      if (OB_ISNULL(header_buf = static_cast<char *>(page_malloc(window)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(reader_->pread(offset_, window, header_buf))) {
      } else {
        ThriftReader r(header_buf, window);
        header = PageHeader();
        read_page_header(r, header);
        if (r.ok()) {
          parsed = true;
          header_len = r.consumed();
        } else if (window >= chunk_end - offset_ || window >= 16L * 1024 * 1024) {
          ret = report("invalid page header");
        } else {
          window = MIN(window * 4, chunk_end - offset_);
        }
      }
      if (OB_NOT_NULL(header_buf)) {
        ob_free(header_buf);
        header_buf = nullptr;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (header.compressed_size_ < 0 || header.uncompressed_size_ < 0
               || header.compressed_size_ > ObFileSqlLimits::MAX_PAGE_SIZE
               || header.uncompressed_size_ > ObFileSqlLimits::MAX_PAGE_SIZE
               || offset_ + header_len + header.compressed_size_ > chunk_end) {
      ret = report("invalid page size");
    } else if (PAGE_DICTIONARY != header.type_ && PAGE_DATA != header.type_ && PAGE_DATA_V2 != header.type_) {
      offset_ += header_len + header.compressed_size_;
    } else {
      const int64_t data_offset = offset_ + header_len;
      char *raw = static_cast<char *>(page_malloc(header.compressed_size_));
      offset_ = data_offset + header.compressed_size_;
      if (OB_ISNULL(raw)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(reader_->pread(data_offset, header.compressed_size_, raw))) {
      } else if (PAGE_DICTIONARY == header.type_) {
        if (has_dict_) {
          ret = report("more than one dictionary page");
        } else if (OB_ISNULL(dict_buf_ = static_cast<char *>(page_malloc(header.uncompressed_size_)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (OB_FAIL(decompress(raw, header.compressed_size_, dict_buf_, header.uncompressed_size_))) {
        } else if (ENC_PLAIN != header.encoding_ && ENC_PLAIN_DICTIONARY != header.encoding_) {
          ret = report("unsupported dictionary encoding");
        } else if (OB_FAIL(decode_values(dict_buf_, header.uncompressed_size_, ENC_PLAIN,
                                         header.num_values_, true))) {
        } else {
          has_dict_ = true;
        }
      } else {
        // data page: definition levels, then values
        const int64_t num_values = header.num_values_;
        const char *values = nullptr;
        int64_t values_len = 0;
        if (num_values <= 0 || num_values > chunk_.num_values_ - values_read_) {
          ret = report("invalid number of values in a page");
        } else if (num_values > MAX_PAGE_VALUES) {
          ret = report("page has too many values");
        } else if (PAGE_DATA == header.type_) {
          if (OB_ISNULL(page_buf_ = static_cast<char *>(page_malloc(header.uncompressed_size_)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(decompress(raw, header.compressed_size_, page_buf_, header.uncompressed_size_))) {
          } else {
            const uint8_t *p = reinterpret_cast<const uint8_t *>(page_buf_);
            int64_t pos = 0;
            if (leaf_.nullable_) {
              int64_t level_len = 0;
              if (ENC_RLE != header.def_encoding_) {
                ret = report("unsupported definition level encoding");
              } else if (header.uncompressed_size_ < 4
                         || (level_len = static_cast<int64_t>(read_le(p, 4))) > header.uncompressed_size_ - 4
                         || !decode_rle(p + 4, p + 4 + level_len, 1, num_values, def_levels_)) {
                ret = report("invalid definition levels");
              } else {
                pos = 4 + level_len;
              }
            }
            values = page_buf_ + pos;
            values_len = header.uncompressed_size_ - pos;
          }
        } else {
          // v2: levels are not compressed, the values may be
          const int64_t levels_len = header.def_length_ + header.rep_length_;
          const int64_t values_raw_len = header.compressed_size_ - levels_len;
          values_len = header.uncompressed_size_ - levels_len;
          if (header.rep_length_ != 0 || header.def_length_ < 0 || values_raw_len < 0 || values_len < 0) {
            ret = report("invalid data page v2");
          } else if (leaf_.nullable_
                     && !decode_rle(reinterpret_cast<const uint8_t *>(raw),
                                    reinterpret_cast<const uint8_t *>(raw) + header.def_length_,
                                    1, num_values, def_levels_)) {
            ret = report("invalid definition levels");
          } else if (OB_ISNULL(page_buf_ = static_cast<char *>(page_malloc(values_len)))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (header.is_compressed_ && CODEC_UNCOMPRESSED != chunk_.codec_) {
            ret = decompress(raw + levels_len, values_raw_len, page_buf_, values_len);
          } else if (values_raw_len != values_len) {
            ret = report("page size mismatch");
          } else {
            MEMCPY(page_buf_, raw + levels_len, values_len);
          }
          values = page_buf_;
        }
        int64_t non_null = num_values;
        if (OB_SUCC(ret) && leaf_.nullable_) {
          non_null = 0;
          for (int64_t i = 0; i < def_levels_.count(); ++i) {
            non_null += def_levels_.at(i) > 0 ? 1 : 0;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (ENC_PLAIN_DICTIONARY == header.encoding_ || ENC_RLE_DICTIONARY == header.encoding_) {
          if (!has_dict_) {
            ret = report("dictionary encoded page without a dictionary");
          } else if (values_len < 1
                     || !decode_rle(reinterpret_cast<const uint8_t *>(values) + 1,
                                    reinterpret_cast<const uint8_t *>(values) + values_len,
                                    static_cast<uint8_t>(values[0]), non_null, ints_)) {
            ret = report("invalid dictionary indexes");
          } else {
            const int64_t dict_size = MAX(dict_ints_.count(), MAX(dict_doubles_.count(), dict_strings_.count()));
            for (int64_t i = 0; OB_SUCC(ret) && i < ints_.count(); ++i) {
              if (ints_.at(i) < 0 || ints_.at(i) >= dict_size) {
                ret = report("dictionary index out of range");
              }
            }
            page_uses_dict_ = true;
          }
        } else if (OB_FAIL(decode_values(values, values_len, header.encoding_, non_null, false))) {
        } else {
          page_uses_dict_ = false;
        }
        if (OB_SUCC(ret)) {
          page_values_ = num_values;
          page_pos_ = 0;
          value_pos_ = 0;
          loaded = true;
        }
      }
      if (OB_NOT_NULL(raw)) {
        ob_free(raw);
      }
    }
  }
  return ret;
}

int ObParquetColumnReader::convert(ObIAllocator &allocator, const int64_t idx, ObFileValue &value)
{
  int ret = OB_SUCCESS;
  const int64_t pos = page_uses_dict_ ? ints_.at(idx) : idx;
  const ObArray<int64_t> &ints = page_uses_dict_ ? dict_ints_ : ints_;
  const ObArray<double> &doubles = page_uses_dict_ ? dict_doubles_ : doubles_;
  const ObArray<ObString> &strings = page_uses_dict_ ? dict_strings_ : strings_;
  value.is_null_ = false;
  if (pos < 0) {
    ret = report("invalid value index");
  } else {
    switch (leaf_.kind_) {
      case PQ_KIND_BOOL:
        value.int_ = ints.at(pos) != 0 ? 1 : 0;
        break;
      case PQ_KIND_INT:
        value.int_ = ints.at(pos);
        if (leaf_.unsigned_ && PQ_INT32 == leaf_.physical_type_) {
          value.int_ = static_cast<uint32_t>(value.int_);
        } else if (leaf_.unsigned_ && value.int_ < 0) {
          ret = report("unsigned value out of the BIGINT range");
        }
        break;
      case PQ_KIND_DOUBLE:
        value.double_ = doubles.at(pos);
        break;
      case PQ_KIND_DATE:
        value.int_ = ints.at(pos);
        break;
      case PQ_KIND_TIMESTAMP: {
        const int64_t v = ints.at(pos);
        if (1000 == leaf_.time_unit_) {
          value.int_ = v * 1000;
        } else if (1000000000 == leaf_.time_unit_) {
          value.int_ = v >= 0 ? v / 1000 : -((-v + 999) / 1000);
        } else {
          value.int_ = v;
        }
        break;
      }
      case PQ_KIND_DECIMAL: {
        long double unscaled = 0;
        if (PQ_INT32 == leaf_.physical_type_ || PQ_INT64 == leaf_.physical_type_) {
          unscaled = static_cast<long double>(ints.at(pos));
        } else {
          const ObString &bytes = strings.at(pos);
          for (int64_t i = 0; i < bytes.length(); ++i) {
            unscaled = unscaled * 256 + static_cast<uint8_t>(bytes.ptr()[i]);
          }
          if (bytes.length() > 0 && (static_cast<uint8_t>(bytes.ptr()[0]) & 0x80)) {
            unscaled -= powl(2.0L, 8 * bytes.length());
          }
        }
        value.double_ = static_cast<double>(unscaled / powl(10.0L, leaf_.scale_));
        break;
      }
      case PQ_KIND_STRING:
        value.str_ = strings.at(pos);
        if (value.str_.length() > ObFileSqlLimits::MAX_VARCHAR_LENGTH) {
          ret = report("value is longer than the VARCHAR limit");
        }
        break;
      case PQ_KIND_TIME: {
        const int64_t v = ints.at(pos);
        const int64_t unit = leaf_.time_unit_;
        const int64_t seconds = v / unit;
        const int64_t micros = (v % unit) * 1000000 / unit;
        char *buf = static_cast<char *>(allocator.alloc(32));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          int64_t len = snprintf(buf, 32, "%02ld:%02ld:%02ld", seconds / 3600, seconds / 60 % 60, seconds % 60);
          if (micros > 0) {
            len += snprintf(buf + len, 32 - len, ".%06ld", micros);
          }
          value.str_.assign_ptr(buf, static_cast<int32_t>(len));
        }
        break;
      }
      case PQ_KIND_UUID: {
        const ObString &bytes = strings.at(pos);
        char *buf = static_cast<char *>(allocator.alloc(40));
        int64_t len = 0;
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t i = 0; i < bytes.length(); ++i) {
            if (4 == i || 6 == i || 8 == i || 10 == i) {
              buf[len++] = '-';
            }
            len += snprintf(buf + len, 40 - len, "%02x", static_cast<uint8_t>(bytes.ptr()[i]));
          }
          value.str_.assign_ptr(buf, static_cast<int32_t>(len));
        }
        break;
      }
      case PQ_KIND_FLOAT16: {
        const ObString &bytes = strings.at(pos);
        value.double_ = half_to_double(static_cast<uint16_t>(read_le(reinterpret_cast<const uint8_t *>(bytes.ptr()), 2)));
        break;
      }
      default:
        ret = report("unknown value kind");
        break;
    }
  }
  return ret;
}

int ObParquetColumnReader::next_value(ObIAllocator &allocator, ObFileValue &value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(file_)) {
    ret = OB_NOT_INIT;
  } else if (page_pos_ >= page_values_) {
    if (values_read_ >= chunk_.num_values_) {
      ret = OB_ITER_END;
    } else if (OB_FAIL(load_page())) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (leaf_.nullable_ && 0 == def_levels_.at(page_pos_)) {
    value.set_null();
  } else {
    const int64_t count = page_uses_dict_ ? ints_.count()
                          : (PQ_KIND_DOUBLE == leaf_.kind_ ? doubles_.count()
                             : (PQ_BYTE_ARRAY == leaf_.physical_type_ || PQ_FIXED_LEN_BYTE_ARRAY == leaf_.physical_type_
                                ? strings_.count() : ints_.count()));
    if (value_pos_ >= count) {
      ret = report("page has fewer values than its definition levels");
    } else {
      ret = convert(allocator, value_pos_++, value);
    }
  }
  if (OB_SUCC(ret)) {
    ++page_pos_;
    ++values_read_;
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase
