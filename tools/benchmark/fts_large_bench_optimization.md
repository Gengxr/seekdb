# Full-text index hot path optimization

## How the bottlenecks were found

Each benchmark workload (IK and BENG `TOKENIZE` loops, `ALTER TABLE ... ADD FULLTEXT INDEX`, and
the four `MATCH` queries) was run against a release build while the observer was sampled with
macOS `sample`. Self time and inclusive time were aggregated per function.

| Workload | Main costs before the change |
|---|---|
| `TOKENIZE(..., 'ik')` | `ObGenDicLoader::get_dic_loader` took a global `TCRWLock` write lock on every call (≈9%); IK dictionary lookups searched their ranges with `ObCharset::strcmp` (≈13%); latch-protected hash maps that only one thread uses (≈5%) |
| Index build | the sort of `(word, doc id)` rows: string comparison was 30–45% of the build, and the `ObCharset::strcmpsp` wrapper (argument checks and charset lookup) cost almost as much as the collation itself |
| `MATCH` queries | the pushed-down `MATCH` filter was evaluated through the expression engine for every document; doc id comparisons in the posting list merge went through the charset layer |
| Fixed DDL latency | a one-row `ADD FULLTEXT INDEX` took 530 ms. An INFO-level trace showed the build threads of each aux index idling up to the 50 ms condition timeout after the dag finished, and the statement polling for completion every 100 ms |

## Changes

| Change | File | Effect |
|---|---|---|
| Look the dictionary loader up under the read lock and take the write lock only to create it | `storage/fts/dict/ob_gen_dic_loader.cpp` | removes the per-call write lock |
| Compare dictionary range bounds by bytes (same order as `utf8mb4_bin`) | `storage/fts/dict/ob_ft_range_dict.cpp` | dictionary lookup no longer goes through the charset dispatch |
| Use hash maps without latches for per-call word maps and IK token chains | `data_plane/.../fts/ob_fts_struct.h`, `storage/fts/ik/ob_ik_arbitrator.h` | no latch per insert |
| Skip lower-casing of words that are already lower case | `storage/fts/ob_fts_stop_word.cpp` | no copy per token |
| Binary search for Chinese number characters | `storage/fts/utils/unicode_utils.cpp` | was a linear scan per character |
| String comparison: equal bytes are equal; binary collation is a `memcmp`; the charset is cached; `utf8mb4_general_ci` compares ASCII and CJK ideographs with its own weight table and leaves other characters to the collation | `share/datum/ob_datum_cmp_func_def.h` | faster sort comparisons in index builds and doc id merges, same results |
| A bare `WHERE MATCH(...) AGAINST(...)` keeps documents with non-zero relevance without evaluating the filter expression | `storage/retrieval/ob_sparse_daat_iter.cpp` | no expression evaluation per document; other filters are still evaluated |
| Wake the threads of an independent dag when it finishes | `storage/scheduler/ob_independent_dag.cpp` | no 50 ms timeout wait per build stage |
| Poll for DDL completion from 1 ms, doubling up to 100 ms | `sql/engine/cmd/ob_ddl_executor_util.cpp` | the statement returns soon after the DDL task finishes |

None of the changes alters tokenization rules, input data or the benchmark.

## Results

`fts_large_bench.sh` with the default parameters (20,000 rows), same machine and load
(macOS arm64 release build, `memory_limit=3G`, `cpu_count=4`):

| Metric | Before | After | Change |
|---|---:|---:|---:|
| build_ik_all_sec | 2.786 | 1.966 | -29.4% |
| build_ik_content_sec | 2.055 | 1.566 | -23.8% |
| build_beng_en_sec | 1.136 | 0.862 | -24.1% |
| tokenize_ik_avg_ms | 0.0706 | 0.0608 | -13.9% |
| tokenize_beng_avg_ms | 0.0446 | 0.0414 | -7.2% |
| query_cn_avg_ms | 0.9526 | 0.8488 | -10.9% |
| query_beng_avg_ms | 1.4133 | 1.2438 | -12.0% |
| query_mixed_avg_ms | 1.0604 | 0.9533 | -10.1% |
| query_limit_avg_ms | 1.2272 | 1.0973 | -10.6% |

A one-row `ADD FULLTEXT INDEX` went from 530 ms to 355 ms.

Query hit counts are unchanged (8001, 11000, 7332, 20).

## Correctness

* `fts_index/fts_hot_path_regression` covers `TOKENIZE` output of both parsers and both IK modes,
  `utf8mb4_general_ci` and binary ordering, grouping and joins over mixed case, accents, trailing
  spaces, CJK and 4-byte characters, and `MATCH` results (ids, counts, scores, `LIMIT`, a score
  filter, after DML). Its result was recorded on the build before the change and passes unchanged.
* The ordering, grouping and join counts of 6,000 random strings, and the full id lists of the
  benchmark queries, are identical before and after the change.
