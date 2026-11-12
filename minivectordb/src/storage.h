// Log-structured row store with primary-key index, per-statement atomicity and
// batched commit durability.
#pragma once

#include "mvd.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace mvd {

constexpr uint32_t kVecRelNone = 0xFFFFFFFFu;
constexpr uint32_t kSlotDeadFlag = 0x80000000u;

struct Slot {
  int64_t pk = 0;
  uint64_t off = 0;        // offset of the encoded row in the log file
  uint64_t arena_off = 0;  // offset in the in-memory arena (valid when table is cached)
  uint32_t row_len = 0;    // high bit: dead flag
  uint32_t vec_rel = kVecRelNone;

  bool dead() const { return (row_len & kSlotDeadFlag) != 0; }
  uint32_t len() const { return row_len & ~kSlotDeadFlag; }
};

// Open addressing hash map: primary key -> slot id.
class PkIndex {
 public:
  void clear();
  size_t size() const { return count_; }
  // Returns slot id or UINT32_MAX.
  uint32_t find(int64_t pk) const;
  // Inserts or returns the existing slot id.
  uint32_t insert_or_get(int64_t pk, uint32_t slot_id, bool& inserted);
  void rehash(size_t capacity);

 private:
  std::vector<int64_t> keys_;
  std::vector<uint32_t> vals_;
  std::vector<uint8_t> used_;
  size_t mask_ = 0;
  size_t count_ = 0;
  void grow();
};

struct Table {
  int32_t id = 0;
  std::string name;
  Schema schema;
  bool has_vector_index = false;

  std::vector<Slot> slots;
  PkIndex pk_index;

  bool cached = false;
  std::vector<char> arena;
  uint64_t arena_live = 0;
  uint64_t arena_garbage = 0;
  uint64_t live_bytes = 0;
  uint64_t live_rows = 0;

  // Lazily built equality indexes (one entry per non-vector column).  A scan
  // with one equality predicate can visit only the matching slots instead of
  // the whole arena.  Entries are appended, never removed, so an updated or
  // deleted row may leave a stale slot id behind; every candidate is
  // re-checked against the row bytes, so staleness costs time but never
  // changes the answer.
  struct EqIndex {
    bool built = false;
    bool disabled = false;  // high-cardinality column: not worth the memory
    std::unordered_map<int64_t, std::vector<uint32_t>> by_int;
    std::unordered_map<std::string, std::vector<uint32_t>> by_str;
  };
  std::vector<EqIndex> eq;

  int32_t vec_idx() const { return schema.find_vec(); }
};

// Overlay entry representing an uncommitted write.
struct OverlayRow {
  int64_t pk = 0;
  bool deleted = false;
  std::vector<char> enc;
  uint32_t vec_rel = kVecRelNone;
};

struct OverlayTable {
  std::vector<OverlayRow> rows;
  std::unordered_map<int64_t, uint32_t> index;  // pk -> index in rows
};

// A row handed to the executor.
struct RowView {
  int64_t pk = 0;
  const char* data = nullptr;
  uint32_t len = 0;
  uint32_t vec_rel = kVecRelNone;
};

class Store {
 public:
  Store(std::string dir, size_t cache_budget);
  ~Store();

  void open();

  Table* find_table(std::string_view name);
  Table& require_table(std::string_view name);

  void create_table(const std::string& name, const Schema& schema);
  void create_vector_index(const std::string& index_name, const std::string& table,
                           const std::string& column);

  // Reads: merged view of committed rows plus uncommitted overlay rows.
  const OverlayTable* overlay_for(const Table& t) const;
  // Returns true when an uncommitted overlay entry shadows this slot.
  const OverlayRow* overlay_find(const Table& t, int64_t pk) const;
  // True when the key is visible either in the committed data or in the overlay.
  bool exists(Table& t, int64_t pk) const;
  // Row bytes for a live committed slot.
  void read_slot_row(const Table& t, const Slot& s, RowView& out);

  // Candidate slot ids for `col = lit`, or nullptr when no equality index
  // applies to this column.  Builds the index on first use.
  const std::vector<uint32_t>* eq_candidates(Table& t, int32_t col, const Value& lit);

  // Transaction control.
  bool in_txn() const { return in_txn_; }
  void begin();
  void rollback();
  void commit();  // appends, fsyncs and applies the pending overlay

  // Staged writes (valid inside an explicit transaction or in autocommit mode
  // before commit()).
  void stage_put(Table& t, const Row& row);
  void stage_delete(Table& t, int64_t pk);
  void clear_pending() { pending_.clear(); }
  bool has_pending() const { return !pending_.empty(); }

  // Called after a successful autocommit statement: the store commits the
  // staged overlay.
  void commit_autocommit();

  void checkpoint_if_needed();

  size_t cache_budget() const { return cache_budget_; }
  const std::vector<std::unique_ptr<Table>>& tables() const { return tables_; }

 private:
  struct Pending {
    int8_t op = 0;  // 1 = put, 2 = del
    Table* table = nullptr;
    int64_t pk = 0;
    Row row;
    std::vector<char> enc;
    uint32_t vec_rel = kVecRelNone;
  };

  std::string dir_;
  size_t cache_budget_;
  int lock_fd_ = -1;
  int log_fd_ = -1;
  uint64_t log_size_ = 0;
  int64_t next_table_id_ = 1;
  std::vector<std::unique_ptr<Table>> tables_;
  std::vector<OverlayTable> overlays_;  // indexed by table id
  bool in_txn_ = false;
  std::vector<Pending> pending_;

  // scratch file reader buffer for non-cached tables
  int read_fd_ = -1;
  std::vector<char> read_buf_;
  uint64_t read_buf_off_ = 0;
  size_t read_buf_len_ = 0;

  void load_catalog();
  void save_catalog();
  void replay_log();
  void build_arenas();
  void apply_put(Table& t, int64_t pk, const char* row, uint32_t len, uint32_t vec_rel,
                 uint64_t file_off, bool from_replay);
  void apply_delete(Table& t, int64_t pk);
  void write_record(const std::vector<Pending>& ops);
  void append_arena(Table& t, Slot& s);
  void compact_log();
  void evict_arenas_if_needed();
  void ensure_buffers();
};

// Encoding helpers.
uint32_t encode_row(const Schema& schema, const Row& row, std::vector<char>& out);
Value decode_column(const Schema& schema, int32_t col, const char* row, uint32_t len);
const char* column_ptr(const Schema& schema, int32_t col, const char* row, uint32_t len, uint32_t* out_len);

}  // namespace mvd
