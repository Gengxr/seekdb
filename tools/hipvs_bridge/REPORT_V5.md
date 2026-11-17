# Compact GPU snapshots (10-iteration baseline)

Parent: `4cf191c5d0df25ed16a1a73f89dc63e2378fc9e9`, reported official score 19.283 (`build=2.836x, query=4.154x, gates=pass`).

This revision preserves that parent's 10 build iterations, graph degrees, FP32 vectors, MULTI_CTA search parameters, ID mapping and persistent worker. It does not inherit the later 5-iteration experiment.

## Changes

* Stage temporary CAGRA files with Linux memfd; fall back to tmpfile if creation fails. Final snapshots are still fully written to seekdb's persistent stream.
* Copy snapshot streams in 1 MiB blocks rather than 64 KiB blocks.
* Persist raw vectors only once, in the outer seekdb snapshot. The hipVS payload now contains the graph without a duplicate dataset. The snapshot header's reserved field is 1 for this layout; 0 remains the legacy full-payload layout and remains readable.
* Reattach the stored FP32 vectors to the loaded graph through the fixed library's `cuvsCagraIndexFromArgs`. This is graph reconstruction, not graph building or CPU search. Keep the staging graph owner and uploaded dataset alive until the reconstructed index is destroyed: the library borrows these device views. Synchronize device copies before returning the handle.
* Retain the legacy bridge serialization/deserialization entry points for existing snapshots and tests.

New snapshots are not readable by old binaries. To roll back to an old binary, rebuild its indexes from the source table instead of reusing the new snapshot files. No cache or graph from a previous evaluation run is reused.

## Executed development checks

The bridge and complete seekdb executable compiled successfully on the supplied AMD ROCm 7.2.4 instance. A graph-only bridge write/read test passed, including incorrect row-count and null-vector rejection. A 250K x 256, nq=4096 synthetic graph-only snapshot/reload test returned unique legal IDs and correct FP32 distances; Recall@10=.967334 and Recall@100=.937129. Its direct-bridge QPS was approximately 1538 / 1545. These are development results, not official dataset results.

Actual SQL tests used a preloaded table with 250,000 FP32 vectors of dimension 128, nonzero external IDs, the specified CREATE VECTOR INDEX parameters, and a first Top-100 query:

| Executable / configuration | CREATE INDEX + first SQL query |
|---|---:|
| Frozen V5 baseline | 21.644 s |
| Passing 10-iteration parent | 6.983 s |
| memfd and larger copy blocks only | 6.763 s |
| Compact snapshots plus those changes | 6.436 s |

The candidate's temporary CAGRA payload was 128,000,921 bytes versus 256,001,713 bytes before this change. The measured elapsed-time reduction relative to the passing parent is about 7.8%, in a single local run per configuration. It is not a statistically established speedup or a guarantee of official build>=3x. GPU snapshot logs confirmed real GPU build and snapshot output. The first SQL query returned 100 unique IDs and the expected self-match ID.

The memory sample collected during the component check includes other running database processes and is not a valid standalone peak-VRAM comparison. Official relative-VRAM, query-regression and profiler checks remain necessary. Official Hidden250K is unavailable; no new official score is claimed. CPU VSAG source is unchanged, but a new full CPU regression run is not claimed.

## Reproduction

Use the contest's unchanged build invocations, compiling this revision's bridge before linking seekdb. `test_graph_snapshot.cpp` uses the same write/read command structure as `test_persistence.cpp`, linked against this bridge. `benchmark_compact.py` is a graph-only variant of the synthetic benchmark; it needs the platform NumPy environment and accepts the same arguments as `benchmark_v2.py`. `sql_build_check.py` prepares and loads the synthetic table outside timing, then measures CREATE INDEX and first query through the mysql CLI. Use separate fresh database directories/processes for comparison. Avoid concurrent compilation or benchmarking during timed runs.
