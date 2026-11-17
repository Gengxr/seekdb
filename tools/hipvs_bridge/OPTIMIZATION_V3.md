# V3: store the FP32 dataset once per GPU snapshot

Parent: `f423bfe34ce370b9bad8830eaeab5ecaeccc03c2`.
The user reports the parent's official result as 6 points:
`build=0.970x, query=1.506x, gates=pass`.

## Change and rationale

The old adaptor writes IDs, raw FP32 vectors, and a native CAGRA blob that
already includes the same FP32 vectors. The compact format writes IDs and that
complete blob only. On reload, the official `cuvsCagraIndexGetDataset` API
exposes the stored device dataset; the bridge copies it into the adaptor's
CPU mirror, respecting row stride. This completes before reload returns.
Appending vectors and existing CPU fallback semantics still have a full mirror.

The existing reserved header field is now a flags field: 0 reads the old
duplicated format, 1 reads the compact format, and other values are rejected.
New code can read old snapshots; old binaries cannot read flag-1 snapshots.
Use clean databases for A/B testing, as required by the contest.

Exact payload reduction is N * dimension * 4 bytes: 128,000,000 bytes for
250,000 x 128 or 256,000,000 bytes for 250,000 x 256. These are arithmetic byte
counts, NOT measured time savings. Reload adds one device-to-host dataset copy;
whether reduced database I/O outweighs that cost requires GPU measurement.

The NN-Descent algorithm, degree 128/256, iteration ceiling 100, FP32 data and
official GPU search parameters are unchanged. All original inputs and graph
contents are retained. V2's worker reuse and ID-bound cache remain in place.
No cross-run graph cache, external dependency or hipVS source modification.

Source checked: fixed hipVS revision
`f96a663d0a4441d6f5344bd3ef552b71ae7b370b`, specifically
`cpp/src/neighbors/detail/cagra/cagra_serialize.cuh`,
`cpp/src/neighbors/cagra_c.cpp`, `cpp/include/cuvs/neighbors/cagra.h`.
Upstream: https://github.com/ROCm-DS/hipVS

## Validation and reproduction

Linux CPU test (Python 3 and GCC 13):

```sh
python3 tools/hipvs_bridge/test_compact_snapshot.py
```

Exit 0 locally with ASan/UBSan: actual adaptor serializer/deserializer extracted
from the working source, fake GPU storage, compact and legacy round trips,
incremental append, truncated payload rejection, unknown flag rejection, and
resource cleanup. The actual bridge dataset-copy function is also tested with
packed/padded rows, byte offsets, invalid strides and injected copy errors.
Bridge syntax was checked against the fixed C API headers with runtime stubs.
This does not validate real GPU behavior or full seekdb compilation.

When the GPU instance is available, use the contest's unchanged bridge and
seekdb build commands. `test_persistence.cpp` additionally checks bitwise FP32
dataset restoration before and after a process restart:

```sh
g++ -std=c++17 tools/hipvs_bridge/test_persistence.cpp \
  -L/path/to/bridge-output -lseekdb_cuvs_bridge \
  -Wl,-rpath,/path/to/bridge-output -o /tmp/cagra-persistence
/tmp/cagra-persistence write /tmp/cagra-index.bin
/tmp/cagra-persistence read /tmp/cagra-index.bin
```

For performance, compare the V5 baseline, V2 and V3 on fresh databases using
the same data and SQL; record total CREATE INDEX + first-query time, recall,
QPS and VRAM. The bridge logs GPU build wall time including synchronization;
the adaptor logs build+native serialization, stream-write and reload times.
Full GPU timing, CPU/GPU comparison, recall and resource results remain pending.
There is no evidence that this change achieves the 3x full-score build target.
