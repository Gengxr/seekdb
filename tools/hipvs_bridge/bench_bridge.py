"""Component-only, reproducible FP32 bridge comparison; not official SQL scoring."""
import ctypes as C
import json
import sys
import time
from concurrent.futures import ThreadPoolExecutor
import numpy as np

lib = C.CDLL(sys.argv[1])
lib.seekdb_cuvs_build.argtypes = [C.c_void_p, C.c_long, C.c_long]
lib.seekdb_cuvs_build.restype = C.c_void_p
lib.seekdb_cuvs_search.argtypes = [C.c_void_p, C.c_void_p, C.c_long, C.c_long, C.c_void_p, C.c_void_p]
lib.seekdb_cuvs_free.argtypes = [C.c_void_p]
base = np.fromfile('/seekdb_workspace/datasets/base.f32', dtype=np.float32).reshape(-1, 128)
query = np.fromfile('/seekdb_workspace/datasets/query.f32', dtype=np.float32).reshape(-1, 128)
t = time.perf_counter()
h = lib.seekdb_cuvs_build(base.ctypes.data, len(base), 128)
assert h
def search(i, k=100, validate=True):
    q = query[i % len(query)]
    ids = np.empty(k, dtype=np.uint32)
    dist = np.empty(k, dtype=np.float32)
    assert lib.seekdb_cuvs_search(h, q.ctypes.data, 1, k, ids.ctypes.data, dist.ctypes.data) == 0
    if validate:
        assert len(np.unique(ids)) == k and np.all(ids < len(base))
        np.testing.assert_allclose(dist, ((base[ids] - q)**2).sum(axis=1), rtol=0.002, atol=0.02)
    return ids
search(0)
build = time.perf_counter() - t
report = {'rows': len(base), 'dim':128, 'build_first_query_s': build}
for k in [10, 100]:
    outputs = [search(i, k) for i in range(len(query))]
    recalls = []
    for q, ids in zip(query, outputs):
        truth = np.argsort(((base-q)**2).sum(axis=1))[:k]
        recalls.append(len(set(truth) & set(ids)) / k)
    t = time.perf_counter()
    for i in range(4096):
        search(i, k, False)
    report[str(k)] = {'recall':float(np.mean(recalls)), 'qps':4096/(time.perf_counter()-t)}
    np.save(sys.argv[2] + str(k) + '.npy', outputs)
if 'candidate' in sys.argv[1]:
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(search, range(32)))
    for nq, k in [(8, 10), (16, 100), (1, 10), (32, 100), (1, 100)]:
        ids = np.empty((nq, k), dtype=np.uint32)
        assert lib.seekdb_cuvs_search(h, query.ctypes.data, nq, k, ids.ctypes.data, None) == 0
        for row in ids:
            assert len(set(row)) == k and np.all(row < len(base))
    assert lib.seekdb_cuvs_search(h, query.ctypes.data, 0, 10, ids.ctypes.data, None) != 0
lib.seekdb_cuvs_free(h)
print(json.dumps(report, indent=2))
