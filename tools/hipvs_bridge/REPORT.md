# V5 fast GPU build and persistent query worker

Frozen base: `634888d9b037153fa143d2c913ba4b25503e1d62`.
Previous submission: `66dd74286271e55c8cc7a967b66e257194e64d51`.
The user reported the previous official result as `build=1.086x, query=1.008x, gates=pass`, score 5.087. That result does not apply to this revision.

## Implementation

* Keep final graph degree 128; use intermediate degree 160 and 30 NN-descent iterations.
* Use the fixed hipVS library's MULTI_CTA CAGRA search, itopk >= 1024 and search width 4. Broader search compensates for cheaper construction. The installed SINGLE_CTA implementation rejects itopk > 512; changing the search mode is essential.
* Reuse one synchronous 32 MiB-stack GPU worker across build, search and snapshot jobs. Callers retain arguments until completion; submissions are serialized; recursive calls execute on the same worker. Shutdown joins the worker. This avoids recreating pthread and HIP thread-local state on each SQL query.
* Retain the previous per-index device buffers, mutex and search-parameter reuse.
* Limit build OpenMP teams to 24 and restore the caller's previous setting afterward. The build script uses ROCm's OpenMP runtime through `-fopenmp`.

The same implementation applies to all inputs and stages. FP32 vectors, IDs and SQL semantics are unchanged. No previous-run graph cache is introduced. Search uses actual hipVS GPU CAGRA results. CPU VSAG code is untouched. CPU exact search appears only in the benchmark oracle.

## Development results

Environment: supplied gfx1100 AMD instance, ROCm 7.2.4, 16-core CPU quota with 128 visible CPUs, about 55 GiB RAM quota. Both libraries run with `OMP_NUM_THREADS=24 OPENBLAS_NUM_THREADS=8`. Each library is measured in a fresh process with independent GPU initialization. Data generation precedes timing. The measured complete bridge time includes build, snapshot write, destruction, snapshot reload and first query.

**These are synthetic direct-bridge results, not SQL end-to-end or official SIFT250K/Hidden250K results.** SQL scanning, ID mapping and DDL overhead are excluded. Raw JSON is under `tools/hipvs_bridge/results/v2/`.

| 250K dataset | Baseline seconds | Candidate seconds | Speedup | Baseline Recall@10 / @100 | Candidate Recall@10 / @100 |
|---|---:|---:|---:|---|---|
| Normal, 128D, nq=4096 | 20.098 | 6.457 | 3.113x | .990601 / .966572 | .996802 / .989114 |
| Normal, 256D, nq=4096 | 20.617 | 6.785 | 3.039x | .949561 / .899500 | .976074 / .950105 |
| 64 clusters, 256D, nq=1000 | 20.431 | 6.785 | 3.011x | .999700 / .999940 | .999700 / .999940 |

| nq=4096 | Baseline Top-10 / Top-100 QPS | Candidate Top-10 / Top-100 QPS | Baseline / candidate peak VRAM bytes |
|---|---|---|---|
| 128D | 306.37 / 292.55 | 1704.94 / 1687.67 | 929406976 / 926453760 |
| 256D | 263.43 / 258.59 | 1588.37 / 1573.37 | 1057685504 / 1054654464 |

VRAM is sampled from sysfs every 20 ms and can miss brief peaks. Peak process RSS stayed below 2.1 GiB. The weighted geometric bridge speedup is approximately 3.07x, **not a competition score or measured SQL speedup**.

## Checks completed

The final bridge compiles with the contest script on the supplied ROCm installation. Snapshot write/read and rejection of incorrect dimensions pass. The 10K platform sample achieves Recall@10/@100=1.0, including concurrent calls on one handle, varying batch sizes, buffer growth, null distance output and invalid query-count rejection. The 250K benchmark checks legal unique IDs, returned FP32 distances and recall against exact ground truth.

`test_worker.cpp` checks 8 concurrent submitters, 8000 completed jobs, nested submission and repeated worker creation/destruction. GPU query ID sets match direct execution on the sample. Final measured fresh-thread QPS was 461.60/467.61 versus persistent-worker 1406.21/1390.63. This is a separate comparison and must not be multiplied into the table's gains.

Rejected experiments include lowering iterations without compensating search quality and default IVF-PQ construction. Those variants are not shipped.

## Reproduce

Compile the baseline bridge from the frozen commit separately; the platform's preinstalled bridge may be older. Compile candidate with the unchanged required invocation:

```bash
HIPVS_ROOT=/opt/hipvs ROCM_ROOT=/opt/rocm tools/hipvs_bridge/build.sh /absolute/path/libseekdb_cuvs_bridge.so
OMP_NUM_THREADS=24 OPENBLAS_NUM_THREADS=8 /opt/venv/bin/python tools/hipvs_bridge/benchmark_v2.py /absolute/path/libseekdb_cuvs_bridge.so --dim 256
# Repeat with --dim 128, swapping baseline/candidate order.
# Extra distribution: --dim 256 --nq 1000 --distribution clustered
```

The Python script needs NumPy, included in the platform virtual environment. It generates deterministic data (seed 917 by default), computes exact truth in bounded blocks, performs 4096 individual queries for each k and prints JSON. No dependency is added to seekdb's production build.

```bash
g++ -std=c++17 -O2 -pthread -Isrc/oblib/lib/vector tools/hipvs_bridge/test_worker.cpp -L/absolute/path -lseekdb_cuvs_bridge -Wl,-rpath,/absolute/path -o /tmp/test_worker
LD_LIBRARY_PATH=/absolute/path:/opt/hipvs/lib:/opt/rocm/lib /tmp/test_worker
```

The worker test uses the platform's `/seekdb_workspace/datasets` sample. Existing `test_persistence.cpp` remains usable with write/read modes. Ensure runtime resolution loads the candidate bridge.

## Remaining official verification

Full seekdb compilation and SQL integration tests have not completed in this session because source downloads were unreliable. The bridge and standalone worker tests were compiled and run. CPU VSAG regression, official GPU profiler audit and paired SIFT250K/Hidden250K tests remain required. No 20-point score is claimed. Keep the previous passing submission as a rollback point until the platform confirms this revision's gates.
