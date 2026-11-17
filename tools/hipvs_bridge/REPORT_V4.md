# V4 build acceleration

Parent: 4cf191c5d0df25ed16a1a73f89dc63e2378fc9e9 (official build=2.836x, query=4.154x, gates=pass).

## Changes

1. Bridge build parameters: NN-descent iterations 10 -> 5 and intermediate graph
   degree 160 -> 128 (final degree stays 128). Cuts GPU graph construction time
   by ~30% versus the parent.
2. Bridge search compensation: MULTI_CTA itopk floor 1024 -> 2048 (search width
   stays 4). Recovers and exceeds the recall lost by the cheaper graph; query
   QPS remains >4x the frozen baseline on every synthetic distribution tested.
3. Snapshot deduplication (OBCUVS03): the serialized cuVS snapshot file already
   embeds the FP32 dataset, so the kernel no longer persists a second raw-vector
   copy alongside it. On load, vectors are recovered from the loaded index via
   the new `seekdb_cuvs_get_dataset` bridge call (cuvsCagraIndexGetDataset +
   device-to-host copy). Snapshot payload shrinks by ~1/3.
4. Snapshot staging file moved to /dev/shm (tmpfs) with tmpfile() fallback, and
   graph copy chunks enlarged 64 KiB -> 1 MiB. Removes the disk round-trip of
   the staging file during DDL persistence.

Search still uses the fixed hipVS MULTI_CTA CAGRA implementation; build still
uses cuvsCagraBuild NN_DESCENT on the GPU. FP32 SQL semantics, IDs, lib=vsag
path and the persistent GPU worker are unchanged. No cross-run graph reuse.

## Development results (synthetic direct-bridge, NOT official SQL scores)

250K rows, nq=1000, seed 917, gfx1100 instance, ROCm 7.2.4. Baseline bridge is
the frozen V5 build (30 NN-descent iterations, SINGLE_CTA itopk 512).

| Dataset | Variant | Build s | Recall@10 | Recall@100 | Top-10 QPS |
|---|---|---:|---:|---:|---:|
| 128D normal | baseline | 19.57 | .9910 | .9658 | 344 |
| 128D normal | parent (10 iter, deg 160, itopk 1024) | 2.99 | .9898 | .9719 | 1641 |
| 128D normal | V4 | 2.47 | .9984 | .9942 | 1500 |
| 256D normal | baseline | 19.34 | .9509 | .8984 | 274 |
| 256D normal | parent | 3.05 | .9623 | .9318 | 1536 |
| 256D normal | V4 | 2.49 | .9904 | .9790 | 1308 |
| 256D clustered | baseline | 18.09 | .9997 | .99994 | ~274 |
| 256D clustered | V4 | 2.62 | .9997 | .99994 | 1215 |

V4 recall is at or above the frozen baseline on every distribution, leaving
margin for the 0.005 baseline-delta gate. Peak process VRAM is unchanged from
the parent (~1.0 GiB; itopk 1024/1536/2048 measured identical peaks). Query
QPS stays >4x baseline, far above the 0.9x regression gate and 1.5x full-score
line.

Rejected alternatives: IVF-PQ build (recall .58), 3-4 NN-descent iterations
without itopk compensation (128D recall delta ~-.0045 vs baseline), intermediate
degree 144 (no recall benefit over 128), search width 8 (weaker recall recovery
than itopk 2048 at equal VRAM).

## SQL end-to-end paired check (synthetic 250K x 128, same instance, same round)

Offline DDL `CREATE VECTOR INDEX ... lib=cuvs` plus first fixed query, timed by
sql_build_check.py against a frozen-baseline server and a V4 server:

| Version | DDL + first query s |
|---|---:|
| frozen baseline | 22.697 |
| V4 candidate | 5.046 |

Local SQL speedup 4.50x (the parent measured 3.10x on this same harness, which
scored official build=2.836x). First query returned 100 unique legal IDs with
the expected top-1. These are synthetic local numbers, not official scores.