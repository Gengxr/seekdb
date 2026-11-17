"""Independent-process GPU bridge benchmark. Synthetic data, NOT official SQL scoring."""
import argparse, ctypes as C, glob, json, os, tempfile, threading, time
import numpy as np
p=argparse.ArgumentParser()
p.add_argument('library');p.add_argument('--dim',type=int,default=256)
p.add_argument('--rows',type=int,default=250000);p.add_argument('--nq',type=int,default=4096)
p.add_argument('--seed',type=int,default=917)
p.add_argument('--distribution',choices=['normal','clustered'],default='normal')
a=p.parse_args();rng=np.random.default_rng(a.seed)
base=rng.standard_normal((a.rows,a.dim),dtype=np.float32)
queries=rng.standard_normal((a.nq,a.dim),dtype=np.float32)
if a.distribution=='clustered':
    centers=4*rng.standard_normal((64,a.dim),dtype=np.float32)
    base+=centers[rng.integers(64,size=a.rows)]
    queries+=centers[rng.integers(64,size=a.nq)]
truth=np.empty((a.nq,100),np.int64);norm=(base*base).sum(1)
for start in range(0,a.nq,64):
    q=queries[start:start+64]
    distances=(q*q).sum(1)[:,None]+norm[None,:]-2*q@base.T
    indices=np.argpartition(distances,99,axis=1)[:,:100]
    order=np.argsort(np.take_along_axis(distances,indices,axis=1),axis=1)
    truth[start:start+len(q)]=np.take_along_axis(indices,order,axis=1)
del distances,indices,order
lib=C.CDLL(os.path.abspath(a.library))
lib.seekdb_cuvs_build.argtypes=[C.c_void_p,C.c_long,C.c_long];lib.seekdb_cuvs_build.restype=C.c_void_p
lib.seekdb_cuvs_search.argtypes=[C.c_void_p,C.c_void_p,C.c_long,C.c_long,C.c_void_p,C.c_void_p]
lib.seekdb_cuvs_free.argtypes=[C.c_void_p]
lib.seekdb_cuvs_serialize.argtypes=[C.c_void_p,C.c_char_p]
lib.seekdb_cuvs_deserialize.argtypes=[C.c_char_p,C.c_long,C.c_long];lib.seekdb_cuvs_deserialize.restype=C.c_void_p
paths=glob.glob('/sys/class/drm/card[0-9]*/device/mem_info_vram_used')
peak=[0];stop=threading.Event()
def monitor():
    while not stop.is_set():
        try: peak[0]=max(peak[0],sum(int(open(path).read()) for path in paths))
        except OSError: pass
        stop.wait(0.02)
thread=threading.Thread(target=monitor);thread.start()
h=None
try:
    t=time.perf_counter();h=lib.seekdb_cuvs_build(base.ctypes.data,a.rows,a.dim);assert h
    build=time.perf_counter()-t
    with tempfile.TemporaryDirectory(prefix='cagra-bench-') as directory:
        path=os.fsencode(os.path.join(directory,'index.cagra'))
        assert lib.seekdb_cuvs_serialize(h,path)==0
        lib.seekdb_cuvs_free(h);h=None
        h=lib.seekdb_cuvs_deserialize(path,a.rows,a.dim);assert h
    first=np.empty(100,np.uint32)
    assert lib.seekdb_cuvs_search(h,queries.ctypes.data,1,100,first.ctypes.data,None)==0
    total=time.perf_counter()-t
    report=dict(library=a.library,rows=a.rows,dim=a.dim,nq=a.nq,seed=a.seed,distribution=a.distribution,
                build_s=build,build_snapshot_reload_first_query_s=total)
    for k in [10,100]:
        ids=np.empty((a.nq,k),np.uint32);dists=np.empty((a.nq,k),np.float32)
        lat=[];start=time.perf_counter()
        for i in range(a.nq):
            t=time.perf_counter()
            rc=lib.seekdb_cuvs_search(h,queries[i].ctypes.data,1,k,ids[i].ctypes.data,dists[i].ctypes.data)
            lat.append(time.perf_counter()-t);assert rc==0
        elapsed=time.perf_counter()-start
        recalls=[]
        for i,row in enumerate(ids):
            assert np.all(row<a.rows) and len(set(row))==k
            exact=((base[row]-queries[i])**2).sum(1)
            np.testing.assert_allclose(dists[i],exact,rtol=0.003,atol=0.03)
            recalls.append(len(set(row)&set(truth[i,:k]))/k)
        report[str(k)]=dict(qps=a.nq/elapsed,p95_ms=float(np.percentile(lat,95)*1000),recall=float(np.mean(recalls)))
    report['device_vram_peak_bytes']=peak[0]
    report['process_peak_rss_kib']=next(line.split()[1] for line in open('/proc/self/status') if line.startswith('VmHWM:'))
    print(json.dumps(report,indent=2),flush=True)
finally:
    if h: lib.seekdb_cuvs_free(h)
    stop.set();thread.join()
