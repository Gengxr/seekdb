// Log-structured row store implementation.
#include "storage.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace mvd {

namespace {

constexpr uint32_t kRecMagic = 0x4D564431u;  // "MVD1"
constexpr uint16_t kRecVersion = 1;
constexpr uint16_t kRecTxn = 1;
constexpr size_t kRecHeaderSize = 24;

void put_u8(std::vector<char>& b, uint8_t v) { b.push_back((char)v); }
void put_u16(std::vector<char>& b, uint16_t v) {
  b.push_back((char)(v & 0xFF));
  b.push_back((char)((v >> 8) & 0xFF));
}
void put_u32(std::vector<char>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF));
}
void put_u64(std::vector<char>& b, uint64_t v) {
  for (int i = 0; i < 8; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF));
}
void put_i64(std::vector<char>& b, int64_t v) { put_u64(b, (uint64_t)v); }

uint8_t rd_u8(const char* p) { return (uint8_t)p[0]; }
uint16_t rd_u16(const char* p) { return (uint16_t)((uint8_t)p[0] | ((uint16_t)(uint8_t)p[1] << 8)); }
uint32_t rd_u32(const char* p) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) v |= ((uint32_t)(uint8_t)p[i]) << (8 * i);
  return v;
}
uint64_t rd_u64(const char* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= ((uint64_t)(uint8_t)p[i]) << (8 * i);
  return v;
}
int64_t rd_i64(const char* p) { return (int64_t)rd_u64(p); }

bool write_full(int fd, const void* data, size_t len, uint64_t off) {
  const char* p = (const char*)data;
  size_t done = 0;
  while (done < len) {
    ssize_t n = ::pwrite(fd, p + done, len - done, (off_t)(off + done));
    if (n <= 0) {
      if (errno == EINTR) continue;
      return false;
    }
    done += (size_t)n;
  }
  return true;
}

bool read_full(int fd, void* data, size_t len, uint64_t off) {
  char* p = (char*)data;
  size_t done = 0;
  while (done < len) {
    ssize_t n = ::pread(fd, p + done, len - done, (off_t)(off + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;
    done += (size_t)n;
  }
  return true;
}

void fsync_dir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

std::string join_path(const std::string& a, const std::string& b) { return a + "/" + b; }

// ---------------------------------------------------------------- catalog I/O
struct CatalogWriter {
  std::vector<char> buf;
  void u32(uint32_t v) { put_u32(buf, v); }
  void i32(int32_t v) { put_u32(buf, (uint32_t)v); }
  void i64(int64_t v) { put_i64(buf, v); }
  void str(const std::string& s) {
    put_u32(buf, (uint32_t)s.size());
    buf.insert(buf.end(), s.begin(), s.end());
  }
};

struct CatalogReader {
  const char* p;
  size_t n;
  size_t i = 0;
  bool ok = true;
  uint32_t u32() {
    if (i + 4 > n) { ok = false; return 0; }
    uint32_t v = rd_u32(p + i);
    i += 4;
    return v;
  }
  int64_t i64() {
    if (i + 8 > n) { ok = false; return 0; }
    int64_t v = rd_i64(p + i);
    i += 8;
    return v;
  }
  std::string str() {
    uint32_t len = u32();
    if (!ok || i + len > n) { ok = false; return {}; }
    std::string s(p + i, len);
    i += len;
    return s;
  }
};

}  // namespace

// ---------------------------------------------------------------- PkIndex
void PkIndex::clear() {
  keys_.clear();
  vals_.clear();
  used_.clear();
  mask_ = 0;
  count_ = 0;
}

void PkIndex::rehash(size_t capacity) {
  size_t cap = 16;
  while (cap < capacity * 2) cap <<= 1;
  std::vector<int64_t> old_keys = std::move(keys_);
  std::vector<uint32_t> old_vals = std::move(vals_);
  std::vector<uint8_t> old_used = std::move(used_);
  keys_.assign(cap, 0);
  vals_.assign(cap, 0);
  used_.assign(cap, 0);
  mask_ = cap - 1;
  count_ = 0;
  for (size_t i = 0; i < old_used.size(); ++i) {
    if (!old_used[i]) continue;
    bool inserted = false;
    insert_or_get(old_keys[i], old_vals[i], inserted);
  }
}

void PkIndex::grow() { rehash(count_ == 0 ? 16 : (count_ + 1) * 2); }

uint32_t PkIndex::find(int64_t pk) const {
  if (mask_ == 0) return UINT32_MAX;
  size_t h = (size_t)((uint64_t)pk * 0x9E3779B97F4A7C15ull) & mask_;
  for (;;) {
    if (!used_[h]) return UINT32_MAX;
    if (keys_[h] == pk) return vals_[h];
    h = (h + 1) & mask_;
  }
}

uint32_t PkIndex::insert_or_get(int64_t pk, uint32_t slot_id, bool& inserted) {
  if (mask_ == 0) grow();
  if ((count_ + 1) * 10 >= (mask_ + 1) * 7) grow();
  size_t h = (size_t)((uint64_t)pk * 0x9E3779B97F4A7C15ull) & mask_;
  for (;;) {
    if (!used_[h]) {
      used_[h] = 1;
      keys_[h] = pk;
      vals_[h] = slot_id;
      ++count_;
      inserted = true;
      return slot_id;
    }
    if (keys_[h] == pk) {
      inserted = false;
      return vals_[h];
    }
    h = (h + 1) & mask_;
  }
}

// ---------------------------------------------------------------- encoding
uint32_t encode_row(const Schema& schema, const Row& row, std::vector<char>& out) {
  out.clear();
  out.reserve(64);
  uint32_t vec_rel = kVecRelNone;
  for (size_t c = 0; c < schema.cols.size(); ++c) {
    const Column& col = schema.cols[c];
    const Value& v = row[c];
    switch (col.type) {
      case ColType::BigInt: {
        put_i64(out, v.i);
        break;
      }
      case ColType::VarChar: {
        put_u32(out, (uint32_t)v.s.size());
        out.insert(out.end(), v.s.begin(), v.s.end());
        break;
      }
      case ColType::Vector: {
        vec_rel = (uint32_t)out.size();
        for (float f : v.v) {
          uint32_t bits;
          std::memcpy(&bits, &f, 4);
          put_u32(out, bits);
        }
        break;
      }
    }
  }
  return vec_rel;
}

const char* column_ptr(const Schema& schema, int32_t col, const char* row, uint32_t len, uint32_t* out_len) {
  const char* p = row;
  const char* end = row + len;
  for (int32_t c = 0; c < col; ++c) {
    switch (schema.cols[c].type) {
      case ColType::BigInt:
        p += 8;
        break;
      case ColType::VarChar: {
        if (p + 4 > end) return nullptr;
        uint32_t sl = rd_u32(p);
        p += 4 + sl;
        break;
      }
      case ColType::Vector:
        p += (size_t)schema.cols[c].len * 4;
        break;
    }
    if (p > end) return nullptr;
  }
  switch (schema.cols[col].type) {
    case ColType::BigInt: {
      if (p + 8 > end) return nullptr;
      if (out_len) *out_len = 8;
      return p;
    }
    case ColType::VarChar: {
      if (p + 4 > end) return nullptr;
      uint32_t sl = rd_u32(p);
      if (p + 4 + sl > end) return nullptr;
      if (out_len) *out_len = sl;
      return p + 4;
    }
    case ColType::Vector: {
      uint32_t need = (uint32_t)schema.cols[col].len * 4;
      if (p + need > end) return nullptr;
      if (out_len) *out_len = need;
      return p;
    }
  }
  return nullptr;
}

Value decode_column(const Schema& schema, int32_t col, const char* row, uint32_t len) {
  Value v;
  v.type = schema.cols[col].type;
  uint32_t n = 0;
  const char* p = column_ptr(schema, col, row, len, &n);
  if (!p) fail(ErrCode::Internal, "corrupt row encoding");
  switch (v.type) {
    case ColType::BigInt:
      v.i = rd_i64(p);
      break;
    case ColType::VarChar:
      v.s.assign(p, n);
      break;
    case ColType::Vector: {
      int32_t dim = schema.cols[col].len;
      v.v.resize(dim);
      for (int32_t i = 0; i < dim; ++i) {
        uint32_t bits = rd_u32(p + 4 * i);
        std::memcpy(&v.v[i], &bits, 4);
      }
      break;
    }
  }
  return v;
}

// ---------------------------------------------------------------- Store
Store::Store(std::string dir, size_t cache_budget) : dir_(std::move(dir)), cache_budget_(cache_budget) {}

Store::~Store() {
  if (log_fd_ >= 0) ::close(log_fd_);
  if (read_fd_ >= 0) ::close(read_fd_);
  if (lock_fd_ >= 0) ::close(lock_fd_);
}

void Store::open() {
  if (::mkdir(dir_.c_str(), 0755) != 0 && errno != EEXIST) {
    fail(ErrCode::IoError, "cannot create data directory " + dir_);
  }
  std::string lock_path = join_path(dir_, "LOCK");
  lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (lock_fd_ < 0) fail(ErrCode::IoError, "cannot open lock file");
  if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
    fail(ErrCode::IoError, "data directory is in use by another process");
  }

  load_catalog();

  std::string log_path = join_path(dir_, "mvd.log");
  log_fd_ = ::open(log_path.c_str(), O_RDWR | O_CREAT, 0644);
  if (log_fd_ < 0) fail(ErrCode::IoError, "cannot open data log");
  struct stat st;
  if (::fstat(log_fd_, &st) != 0) fail(ErrCode::IoError, "cannot stat data log");
  log_size_ = (uint64_t)st.st_size;

  read_fd_ = ::dup(log_fd_);
  read_buf_.resize(4u << 20);

  replay_log();
  build_arenas();
  log_size_ = (uint64_t)::lseek(log_fd_, 0, SEEK_END);
}

void Store::load_catalog() {
  std::string path = join_path(dir_, "catalog.mvd");
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    tables_.clear();
    overlays_.assign(2, OverlayTable{});
    next_table_id_ = 1;
    return;
  }
  struct stat st;
  ::fstat(fd, &st);
  std::vector<char> data((size_t)st.st_size);
  if (!data.empty() && !read_full(fd, data.data(), data.size(), 0)) {
    ::close(fd);
    fail(ErrCode::IoError, "cannot read catalog");
  }
  ::close(fd);
  if (data.size() < 12) fail(ErrCode::IoError, "catalog is corrupt");
  CatalogReader r{data.data(), data.size(), 0, true};
  r.u32();  // magic
  r.u32();  // version
  next_table_id_ = r.i64();
  uint32_t ntables = r.u32();
  if (!r.ok) fail(ErrCode::IoError, "catalog is corrupt");
  tables_.clear();
  overlays_.clear();
  int32_t max_id = 1;
  for (uint32_t i = 0; i < ntables; ++i) {
    auto t = std::make_unique<Table>();
    t->id = (int32_t)r.u32();
    t->has_vector_index = r.u32() != 0;
    t->name = r.str();
    uint32_t ncols = r.u32();
    for (uint32_t c = 0; c < ncols; ++c) {
      Column col;
      col.type = (ColType)r.u32();
      col.len = (int32_t)r.u32();
      col.name = r.str();
      t->schema.cols.push_back(std::move(col));
    }
    t->schema.pk_idx = (int32_t)r.u32();
    t->schema.vec_idx = t->schema.find_vec();
    if (!r.ok) fail(ErrCode::IoError, "catalog is corrupt");
    max_id = std::max(max_id, t->id + 1);
    tables_.push_back(std::move(t));
  }
  if (next_table_id_ < max_id) next_table_id_ = max_id;
  overlays_.resize((size_t)next_table_id_ + 1);
}

void Store::save_catalog() {
  CatalogWriter w;
  w.u32(0x4D564443u);  // "MVDC"
  w.u32(1);
  w.i64(next_table_id_);
  w.u32((uint32_t)tables_.size());
  for (const auto& t : tables_) {
    w.u32((uint32_t)t->id);
    w.u32(t->has_vector_index ? 1u : 0u);
    w.str(t->name);
    w.u32((uint32_t)t->schema.cols.size());
    for (const auto& c : t->schema.cols) {
      w.u32((uint32_t)c.type);
      w.u32((uint32_t)c.len);
      w.str(c.name);
    }
    w.u32((uint32_t)t->schema.pk_idx);
  }
  std::string path = join_path(dir_, "catalog.mvd");
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) fail(ErrCode::IoError, "cannot write catalog");
  bool ok = write_full(fd, w.buf.data(), w.buf.size(), 0);
  if (ok) ::fdatasync(fd);
  ::close(fd);
  if (!ok) fail(ErrCode::IoError, "cannot write catalog");
  if (::rename(tmp.c_str(), path.c_str()) != 0) fail(ErrCode::IoError, "cannot commit catalog");
  fsync_dir(dir_);
}

Table* Store::find_table(std::string_view name) {
  for (auto& t : tables_) {
    if (iequals(t->name, name)) return t.get();
  }
  return nullptr;
}

Table& Store::require_table(std::string_view name) {
  Table* t = find_table(name);
  if (!t) fail(ErrCode::TableNotFound, "table not found: " + std::string(name));
  return *t;
}

void Store::create_table(const std::string& name, const Schema& schema) {
  if (in_txn_) fail(ErrCode::UnsupportedSql, "DDL inside a transaction is not supported");
  if (find_table(name)) fail(ErrCode::TableExists, "table already exists: " + name);
  auto t = std::make_unique<Table>();
  t->id = (int32_t)next_table_id_++;
  t->name = name;
  t->schema = schema;
  t->cached = true;  // new tables start small and are cached until evicted
  tables_.push_back(std::move(t));
  overlays_.resize((size_t)next_table_id_ + 1);
  save_catalog();
}

void Store::create_vector_index(const std::string& index_name, const std::string& table_name,
                                const std::string& column) {
  if (in_txn_) fail(ErrCode::UnsupportedSql, "DDL inside a transaction is not supported");
  Table& t = require_table(table_name);
  int32_t ci = t.schema.find(column);
  if (ci < 0) fail(ErrCode::ColumnNotFound, "column not found: " + column);
  if (t.schema.cols[ci].type != ColType::Vector) {
    fail(ErrCode::InvalidValue, "vector index requires a VECTOR column");
  }
  if (t.has_vector_index) fail(ErrCode::IndexExists, "vector index already exists on " + t.name);
  (void)index_name;
  t.has_vector_index = true;
  save_catalog();
}

const OverlayTable* Store::overlay_for(const Table& t) const {
  if ((size_t)t.id >= overlays_.size()) return nullptr;
  return &overlays_[t.id];
}

const OverlayRow* Store::overlay_find(const Table& t, int64_t pk) const {
  if ((size_t)t.id >= overlays_.size()) return nullptr;
  const OverlayTable& ov = overlays_[t.id];
  auto it = ov.index.find(pk);
  if (it == ov.index.end()) return nullptr;
  return &ov.rows[it->second];
}

bool Store::exists(Table& t, int64_t pk) const {
  if ((size_t)t.id < overlays_.size()) {
    const OverlayTable& ov = overlays_[t.id];
    auto it = ov.index.find(pk);
    if (it != ov.index.end()) return !ov.rows[it->second].deleted;
  }
  uint32_t sid = t.pk_index.find(pk);
  return sid != UINT32_MAX && !t.slots[sid].dead();
}

void Store::ensure_buffers() {
  if (read_buf_.empty()) read_buf_.resize(4u << 20);
}

void Store::read_slot_row(const Table& t, const Slot& s, RowView& out) {
  out.pk = s.pk;
  out.len = s.len();
  out.vec_rel = s.vec_rel;
  if (t.cached) {
    out.data = t.arena.data() + s.arena_off;
    return;
  }
  ensure_buffers();
  if (s.off >= read_buf_off_ && s.off + out.len <= read_buf_off_ + read_buf_len_) {
    out.data = read_buf_.data() + (s.off - read_buf_off_);
    return;
  }
  size_t want = std::max<size_t>(out.len, read_buf_.size());
  if (s.off + want > log_size_) want = (size_t)(log_size_ - s.off);
  if (want < out.len) fail(ErrCode::IoError, "log truncated while reading row");
  if (!read_full(read_fd_, read_buf_.data(), want, s.off)) {
    fail(ErrCode::IoError, "log read failed");
  }
  read_buf_off_ = s.off;
  read_buf_len_ = want;
  out.data = read_buf_.data();
}

namespace {

// Append one slot id to an equality index.  The row is already in the log
// encoding, so the key is decoded straight out of those bytes.
void eq_index_add(const Table& t, Table::EqIndex& ix, int32_t col, uint32_t slot, const char* row,
                  uint32_t len) {
  uint32_t vlen = 0;
  const char* p = column_ptr(t.schema, col, row, len, &vlen);
  if (!p) return;
  if (t.schema.cols[(size_t)col].type == ColType::BigInt) {
    int64_t key = 0;
    std::memcpy(&key, p, sizeof key);
    ix.by_int[key].push_back(slot);
  } else {
    ix.by_str[std::string(p, vlen)].push_back(slot);
  }
}

}  // namespace

void Store::apply_put(Table& t, int64_t pk, const char* row, uint32_t len, uint32_t vec_rel,
                      uint64_t file_off, bool from_replay) {
  uint32_t sid = t.pk_index.find(pk);
  bool fresh = sid == UINT32_MAX;
  uint32_t old_len = 0;
  bool old_live = false;
  if (!fresh) {
    Slot& s = t.slots[sid];
    old_live = !s.dead();
    old_len = s.len();
    if (!from_replay && old_live && t.cached) t.arena_garbage += old_len;
  }
  if (fresh) {
    Slot s;
    sid = (uint32_t)t.slots.size();
    t.slots.push_back(s);
    bool inserted = false;
    t.pk_index.insert_or_get(pk, sid, inserted);
  }
  Slot& s = t.slots[sid];
  if (!from_replay) {
    if (old_live) {
      t.live_bytes -= old_len;
      --t.live_rows;
    }
  } else if (old_live) {
    t.live_bytes -= old_len;
    --t.live_rows;
  }
  s.pk = pk;
  s.off = file_off;
  s.row_len = len;
  s.vec_rel = vec_rel;
  t.live_bytes += len;
  ++t.live_rows;
  if (!from_replay && t.cached) {
    s.arena_off = t.arena.size();
    t.arena.insert(t.arena.end(), row, row + len);
    t.arena_live += len;
  } else if (from_replay) {
    s.arena_off = 0;
  }
  // Keep any already-built equality index in step with the committed rows.
  for (size_t c = 0; c < t.eq.size(); ++c) {
    if (t.eq[c].built && !t.eq[c].disabled) eq_index_add(t, t.eq[c], (int32_t)c, sid, row, len);
  }
}

const std::vector<uint32_t>* Store::eq_candidates(Table& t, int32_t col, const Value& lit) {
  static const std::vector<uint32_t> kNone;
  if (col < 0 || (size_t)col >= t.schema.cols.size() || !t.cached) return nullptr;
  const Column& column = t.schema.cols[(size_t)col];
  if (column.type == ColType::Vector) return nullptr;
  if (t.eq.size() != t.schema.cols.size()) t.eq.resize(t.schema.cols.size());
  Table::EqIndex& ix = t.eq[(size_t)col];
  if (ix.disabled) return nullptr;
  if (!ix.built) {
    for (uint32_t i = 0; i < (uint32_t)t.slots.size(); ++i) {
      const Slot& s = t.slots[i];
      if (s.dead()) continue;
      eq_index_add(t, ix, col, i, t.arena.data() + s.arena_off, s.len());
    }
    ix.built = true;
    if (ix.by_str.size() > 4096) {
      // A near-unique column would cost more memory than the scan it saves.
      ix.disabled = true;
      std::unordered_map<std::string, std::vector<uint32_t>>().swap(ix.by_str);
      std::unordered_map<int64_t, std::vector<uint32_t>>().swap(ix.by_int);
      return nullptr;
    }
  }
  if (column.type == ColType::BigInt) {
    auto it = ix.by_int.find(lit.i);
    return it == ix.by_int.end() ? &kNone : &it->second;
  }
  auto it = ix.by_str.find(lit.s);
  return it == ix.by_str.end() ? &kNone : &it->second;
}

void Store::apply_delete(Table& t, int64_t pk) {
  uint32_t sid = t.pk_index.find(pk);
  if (sid == UINT32_MAX) return;
  Slot& s = t.slots[sid];
  if (s.dead()) return;
  if (t.cached) t.arena_garbage += s.len();
  t.live_bytes -= s.len();
  --t.live_rows;
  s.row_len |= kSlotDeadFlag;
}

void Store::replay_log() {
  if (log_size_ == 0) return;
  const size_t map_len = (size_t)log_size_;
  void* map = ::mmap(nullptr, map_len, PROT_READ, MAP_PRIVATE, log_fd_, 0);
  const char* base = nullptr;
  std::vector<char> fallback;
  if (map == MAP_FAILED) {
    fallback.resize(map_len);
    if (!read_full(log_fd_, fallback.data(), fallback.size(), 0)) fail(ErrCode::IoError, "log read failed");
    base = fallback.data();
  } else {
    base = (const char*)map;
  }
  uint64_t valid_end = 0;
  uint64_t off = 0;
  while (off + kRecHeaderSize <= log_size_) {
    const char* h = base + off;
    uint32_t magic = rd_u32(h);
    if (magic != kRecMagic) break;
    uint16_t version = rd_u16(h + 4);
    uint16_t type = rd_u16(h + 6);
    uint64_t len = rd_u64(h + 8);
    uint32_t crc = rd_u32(h + 16);
    if (version != kRecVersion || type != kRecTxn) break;
    if (off + kRecHeaderSize + len > log_size_) break;
    const char* payload = h + kRecHeaderSize;
    if (crc32_bytes(payload, len) != crc) break;
    // parse payload
    if (len < 4) break;
    uint32_t nops = rd_u32(payload);
    const char* q = payload + 4;
    const char* qend = payload + len;
    bool corrupt = false;
    struct ReplayOp {
      uint8_t op;
      int32_t table;
      int64_t pk;
      uint32_t rowlen;
      uint32_t vec_rel;
      uint64_t row_off;
    };
    std::vector<ReplayOp> ops;
    ops.reserve(nops);
    for (uint32_t i = 0; i < nops; ++i) {
      if (q + 1 + 4 + 8 + 4 + 4 > qend) { corrupt = true; break; }
      ReplayOp op;
      op.op = rd_u8(q);
      op.table = (int32_t)rd_u32(q + 1);
      op.pk = rd_i64(q + 5);
      op.rowlen = rd_u32(q + 13);
      op.vec_rel = rd_u32(q + 17);
      q += 21;
      op.row_off = (uint64_t)(q - base);
      if (q + op.rowlen > qend) { corrupt = true; break; }
      q += op.rowlen;
      ops.push_back(op);
    }
    if (corrupt || q != qend) break;
    for (const ReplayOp& op : ops) {
      Table* t = nullptr;
      for (auto& tt : tables_) {
        if (tt->id == op.table) { t = tt.get(); break; }
      }
      if (!t) continue;
      if (op.op == 1) {
        apply_put(*t, op.pk, base + op.row_off, op.rowlen, op.vec_rel, op.row_off, true);
      } else if (op.op == 2) {
        apply_delete(*t, op.pk);
      }
    }
    off += kRecHeaderSize + len;
    valid_end = off;
  }
  if (valid_end < log_size_) {
    if (::ftruncate(log_fd_, (off_t)valid_end) != 0) fail(ErrCode::IoError, "cannot truncate torn log tail");
    log_size_ = valid_end;
  }
  if (map != MAP_FAILED) ::munmap(map, map_len);
}

void Store::build_arenas() {
  // Decide which tables can be fully cached, largest first.
  std::vector<Table*> order;
  order.reserve(tables_.size());
  for (auto& t : tables_) order.push_back(t.get());
  std::sort(order.begin(), order.end(), [](Table* a, Table* b) { return a->live_bytes < b->live_bytes; });
  size_t budget = cache_budget_;
  for (Table* t : order) {
    if (t->live_bytes > 0 && t->live_bytes <= budget) {
      t->cached = true;
      budget -= t->live_bytes;
    }
  }
  for (Table* t : order) {
    if (!t->cached) continue;
    t->arena.clear();
    t->arena.reserve((size_t)t->live_bytes + (size_t)t->live_bytes / 8 + 4096);
    t->arena_live = 0;
    t->arena_garbage = 0;
    for (Slot& s : t->slots) {
      if (s.dead()) continue;
      ensure_buffers();
      uint32_t len = s.len();
      size_t want = std::max<size_t>(len, read_buf_.size());
      if (s.off + want > log_size_) want = (size_t)(log_size_ - s.off);
      if (want < len || !read_full(read_fd_, read_buf_.data(), want, s.off)) {
        fail(ErrCode::IoError, "log read failed while building cache");
      }
      read_buf_off_ = s.off;
      read_buf_len_ = want;
      s.arena_off = t->arena.size();
      t->arena.insert(t->arena.end(), read_buf_.data(), read_buf_.data() + len);
      t->arena_live += len;
    }
  }
}

void Store::begin() {
  if (in_txn_) fail(ErrCode::TransactionActive, "a transaction is already active");
  in_txn_ = true;
  for (auto& ov : overlays_) {
    ov.rows.clear();
    ov.index.clear();
  }
}

void Store::rollback() {
  in_txn_ = false;
  for (auto& ov : overlays_) {
    ov.rows.clear();
    ov.index.clear();
  }
}

void Store::stage_put(Table& t, const Row& row) {
  int64_t pk = row[t.schema.pk_idx].i;
  OverlayTable& ov = overlays_[t.id];
  auto it = ov.index.find(pk);
  if (it == ov.index.end()) {
    OverlayRow r;
    r.pk = pk;
    r.vec_rel = encode_row(t.schema, row, r.enc);
    ov.index.emplace(pk, (uint32_t)ov.rows.size());
    ov.rows.push_back(std::move(r));
  } else {
    OverlayRow& r = ov.rows[it->second];
    r.pk = pk;
    r.deleted = false;
    r.vec_rel = encode_row(t.schema, row, r.enc);
  }
}

void Store::stage_delete(Table& t, int64_t pk) {
  OverlayTable& ov = overlays_[t.id];
  auto it = ov.index.find(pk);
  if (it == ov.index.end()) {
    OverlayRow r;
    r.pk = pk;
    r.deleted = true;
    ov.index.emplace(pk, (uint32_t)ov.rows.size());
    ov.rows.push_back(std::move(r));
  } else {
    OverlayRow& r = ov.rows[it->second];
    r.deleted = true;
    r.enc.clear();
    r.vec_rel = kVecRelNone;
  }
}

void Store::write_record(const std::vector<Pending>& ops) {
  // payload
  std::vector<char> payload;
  size_t estimate = 4;
  for (const auto& op : ops) estimate += 21 + op.enc.size();
  payload.reserve(estimate);
  put_u32(payload, (uint32_t)ops.size());
  struct Fixup {
    size_t payload_off;
    size_t op_index;
  };
  std::vector<size_t> row_offsets(ops.size(), 0);
  for (size_t i = 0; i < ops.size(); ++i) {
    const Pending& op = ops[i];
    put_u8(payload, (uint8_t)(op.op == 1 ? 1 : 2));
    put_u32(payload, (uint32_t)op.table->id);
    put_i64(payload, op.pk);
    put_u32(payload, (uint32_t)(op.op == 1 ? op.enc.size() : 0));
    put_u32(payload, op.op == 1 ? op.vec_rel : kVecRelNone);
    row_offsets[i] = payload.size();
    if (op.op == 1) payload.insert(payload.end(), op.enc.begin(), op.enc.end());
  }
  std::vector<char> record;
  record.reserve(kRecHeaderSize + payload.size());
  put_u32(record, kRecMagic);
  put_u16(record, kRecVersion);
  put_u16(record, kRecTxn);
  put_u64(record, (uint64_t)payload.size());
  put_u32(record, crc32_bytes(payload.data(), payload.size()));
  put_u32(record, 0);
  record.insert(record.end(), payload.begin(), payload.end());
  uint64_t start = log_size_;
  if (!write_full(log_fd_, record.data(), record.size(), start)) {
    fail(ErrCode::IoError, "cannot append to data log");
  }
  if (::fdatasync(log_fd_) != 0) fail(ErrCode::IoError, "cannot sync data log");
  log_size_ = start + record.size();
  // Apply to the in-memory state only after the record is durable.
  for (size_t i = 0; i < ops.size(); ++i) {
    const Pending& op = ops[i];
    if (op.op == 1) {
      uint64_t row_off = start + kRecHeaderSize + row_offsets[i];
      apply_put(*op.table, op.pk, op.enc.data(), (uint32_t)op.enc.size(), op.vec_rel, row_off, false);
    } else {
      apply_delete(*op.table, op.pk);
    }
  }
}

void Store::commit() {
  std::vector<Pending> ops;
  for (auto& t : tables_) {
    OverlayTable& ov = overlays_[t->id];
    if (ov.rows.empty()) continue;
    for (auto& r : ov.rows) {
      Pending p;
      p.table = t.get();
      p.pk = r.pk;
      if (r.deleted) {
        p.op = 2;
      } else {
        p.op = 1;
        p.enc = std::move(r.enc);
        p.vec_rel = r.vec_rel;
      }
      ops.push_back(std::move(p));
    }
    ov.rows.clear();
    ov.index.clear();
  }
  in_txn_ = false;
  if (ops.empty()) return;
  write_record(ops);
  evict_arenas_if_needed();
  checkpoint_if_needed();
}

void Store::commit_autocommit() { commit(); }

void Store::evict_arenas_if_needed() {
  size_t total = 0;
  for (auto& t : tables_) {
    if (t->cached) total += t->arena.size();
  }
  while (total > cache_budget_) {
    Table* victim = nullptr;
    for (auto& t : tables_) {
      if (t->cached && t->arena.size() > (victim ? victim->arena.size() : 0)) victim = t.get();
    }
    if (!victim) break;
    total -= victim->arena.size();
    victim->cached = false;
    victim->arena.clear();
    victim->arena.shrink_to_fit();
    victim->arena_live = 0;
    victim->arena_garbage = 0;
  }
}

void Store::checkpoint_if_needed() {
  // Rewrite the log when it is dominated by dead versions.
  uint64_t live = 0;
  for (auto& t : tables_) live += t->live_bytes;
  if (log_size_ > (uint64_t)64 * 1024 * 1024 && log_size_ > live * 4 + (uint64_t)8 * 1024 * 1024) {
    compact_log();
  }
  for (auto& t : tables_) {
    if (!t->cached) continue;
    if (t->arena.size() > (size_t)8 * 1024 * 1024 && t->arena_garbage * 2 > t->arena.size()) {
      // Compact the arena in place.
      std::vector<char> fresh;
      fresh.reserve((size_t)t->arena_live + 4096);
      for (Slot& s : t->slots) {
        if (s.dead()) continue;
        uint64_t old_off = s.arena_off;
        s.arena_off = fresh.size();
        fresh.insert(fresh.end(), t->arena.data() + old_off, t->arena.data() + old_off + s.len());
      }
      t->arena.swap(fresh);
      t->arena_garbage = 0;
    }
  }
}

void Store::compact_log() {
  std::string tmp = join_path(dir_, "mvd.log.tmp");
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) fail(ErrCode::IoError, "cannot create compacted log");
  uint64_t off = 0;
  // Write in batches to bound memory.
  const size_t kBatchRows = 512;
  size_t i = 0;
  std::vector<char> payload;
  std::vector<std::pair<Table*, Slot*>> batch;
  auto flush_batch = [&]() {
    if (batch.empty()) return;
    payload.clear();
    put_u32(payload, (uint32_t)batch.size());
    std::vector<uint64_t> row_offsets;
    for (auto& bs : batch) {
      RowView rv;
      read_slot_row(*bs.first, *bs.second, rv);
      put_u8(payload, 1);
      put_u32(payload, (uint32_t)bs.first->id);
      put_i64(payload, rv.pk);
      put_u32(payload, rv.len);
      put_u32(payload, rv.vec_rel);
      row_offsets.push_back(payload.size());
      payload.insert(payload.end(), rv.data, rv.data + rv.len);
    }
    std::vector<char> record;
    record.reserve(kRecHeaderSize + payload.size());
    put_u32(record, kRecMagic);
    put_u16(record, kRecVersion);
    put_u16(record, kRecTxn);
    put_u64(record, (uint64_t)payload.size());
    put_u32(record, crc32_bytes(payload.data(), payload.size()));
    put_u32(record, 0);
    record.insert(record.end(), payload.begin(), payload.end());
    if (!write_full(fd, record.data(), record.size(), off)) fail(ErrCode::IoError, "log compaction write failed");
    off += record.size();
    batch.clear();
  };
  for (auto& t : tables_) {
    for (Slot& s : t->slots) {
      if (s.dead()) continue;
      batch.emplace_back(t.get(), &s);
      if (batch.size() >= kBatchRows) flush_batch();
      ++i;
    }
  }
  (void)i;
  flush_batch();
  ::fdatasync(fd);
  ::close(fd);
  if (::rename(tmp.c_str(), join_path(dir_, "mvd.log").c_str()) != 0) {
    fail(ErrCode::IoError, "cannot install compacted log");
  }
  fsync_dir(dir_);
  // Reopen and rebuild pointers.
  ::close(log_fd_);
  if (read_fd_ >= 0) ::close(read_fd_);
  log_fd_ = ::open(join_path(dir_, "mvd.log").c_str(), O_RDWR);
  if (log_fd_ < 0) fail(ErrCode::IoError, "cannot reopen compacted log");
  read_fd_ = ::dup(log_fd_);
  log_size_ = off;
  // Slots keep referencing the old file; rebuild the whole in-memory state.
  for (auto& t : tables_) {
    t->slots.clear();
    t->pk_index.clear();
    t->live_bytes = 0;
    t->live_rows = 0;
    t->cached = false;
    t->arena.clear();
    t->arena_live = 0;
    t->arena_garbage = 0;
    // Slot ids are reassigned by the replay below, so any cached candidate list
    // would point at the wrong rows.  Drop it and let it rebuild on first use.
    t->eq.clear();
  }
  read_buf_off_ = 0;
  read_buf_len_ = 0;
  replay_log();
  build_arenas();
}

}  // namespace mvd
