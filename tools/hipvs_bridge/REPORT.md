# V5 GPU scratch-buffer reuse: development submission

Base: `634888d9b037153fa143d2c913ba4b25503e1d62`.

This is an initial, tested bridge optimization, **not a completed build-speedup submission**. No official score, CPU/GPU build comparison, or 250K end-to-end speedup is claimed.

## Implementation

The bridge retains query, ID and distance device buffers per index, grows them only when necessary, and releases them with the index. Search parameters are retained and reset for each requested k. A per-index mutex serializes search and snapshot serialization. Different index handles do not share buffers. New handles and deserialized snapshots start with empty buffers; no previous-run graph or cache is reused. Invalid sizes and distance-copy failures return errors.

NN-descent iterations (100), graph degrees (128/256), FP32 data, search width (4) and minimum itopk (512) remain unchanged. The official hipVS search implementation remains in use. No changes are made to the CPU VSAG path or SQL interface. Destruction requires callers to finish all operations first, as documented in the header.

## Measured component results

Environment: supplied AMD platform instance, gfx1100, ROCm 7.2.4, installed `/opt/hipvs`. Platform sample: 10,000 rows x 128 FP32, 100 queries. Each throughput measurement uses 4096 sequential calls; validation occurs outside the timed loop. These are direct bridge calls, **not SQL queries**. One candidate-then-baseline pair:

| Metric | Frozen bridge | Candidate |
|---|---:|---:|
| Top-10 queries/s | 377.282 | 380.434 |
| Top-100 queries/s | 378.856 | 382.198 |
| Recall@10 | 1.000 | 1.000 |
| Recall@100 | 1.000 | 1.000 |
| Build plus first query, seconds | 5.379 | 7.088 |

The small throughput difference is not evidence of a statistically significant gain. The build algorithm is unchanged; this pair did not show a build improvement. Compilation of the bridge, snapshot write/read, rejection of incorrect snapshot dimensions, result ID uniqueness/range and L2 distance checks passed. Four-thread searches on a shared handle also passed. The test covers buffer growth, smaller subsequent requests, null distance output and invalid query counts.

## Reproduce

Compile the frozen bridge separately from the frozen commit using the contest build script. Compile this bridge using the same command:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /absolute/path/libcandidate.so
python tools/hipvs_bridge/bench_bridge.py /absolute/path/libcandidate.so candidate
python tools/hipvs_bridge/bench_bridge.py /absolute/path/libbaseline.so baseline
```

Python requires NumPy; it is available in the supplied platform's `/opt/venv/bin/python`. The script uses the platform sample files under `/seekdb_workspace/datasets`, computes exact FP32 ground truth on CPU solely for test validation, and saves returned IDs for inspection. It does not participate in seekdb execution or official evaluation. Repeat paired runs with reversed order before making performance claims.

The existing `test_persistence.cpp` can be compiled against this bridge and run with `write` and `read` arguments. Ensure `LD_LIBRARY_PATH` resolves `libseekdb_cuvs_bridge.so` to the candidate, not the platform's preinstalled library.

## Outstanding validation and work

Full seekdb source compilation and SQL integration tests have not completed: full Git source download stalled on this instance. No CPU VSAG regression test, GPU profiler audit, peak VRAM measurement or official SIFT250K/Hidden250K paired run has been performed. These remain required before treating the branch as competition-ready. The retained scratch footprint is proportional to the largest query/result shapes seen by each live index and must be included in the VRAM audit. Primary CAGRA build optimization remains outstanding.
