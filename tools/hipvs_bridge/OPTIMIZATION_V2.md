# V2: remove per-query CPU overhead

Parent: `1b491b022451b4448dc20e86e998d9c74aeb2b12`.
User-reported official parent result: 4.925 points,
build=1.068x, query=1.017x, gates=pass.

Changes:
- Reuse one synchronous 32 MiB-stack worker for bridge jobs instead of creating
  and joining a pthread for each SQL query. The caller waits for completion;
  no first-query work is deferred beyond its return.
- Cache external-ID bounds per index entry. Only newly appended IDs are scanned.
  Replacing or deserializing an entry resets this cache. No query results or
  graphs are reused across index lifetimes.
- Log monotonic wall time for actual GPU graph construction, including sync,
  to distinguish it from the adaptor's existing snapshot timing.

FP32 inputs, graph degree, construction iteration limit, search parameters,
GPU algorithms and snapshot format are unchanged. CPU VSAG paths are unchanged.
This iteration primarily targets query overhead; it does not establish the
3x construction speedup needed for full marks.

Local validation (WSL Ubuntu 24.04, GCC 13):

```sh
g++ -std=c++17 -pthread -fsanitize=address,undefined \
  tools/hipvs_bridge/test_worker.cpp -o /tmp/cuvs-worker-test
/tmp/cuvs-worker-test
```

Exit 0: 2,400 concurrent submissions, correct return values, same worker thread,
2 MiB stack use and three worker creation/shutdown cycles.
Bridge syntax also checked against the fixed hipVS C headers using a runtime
declaration stub. This is NOT a real ROCm build or GPU test.

No local AMD GPU is available. Full seekdb compilation, Recall, VRAM usage,
GPU-path validation and end-to-end speedup still require the official environment.
No score or performance improvement is claimed for this candidate yet.
