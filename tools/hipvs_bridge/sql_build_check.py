import subprocess,time,json,sys,numpy as np
root='/workspace/work/'
mode=sys.argv[1]
def sql(text,port=2881):
 r=subprocess.run(['mysql','--local-infile=1','-h127.0.0.1','-P'+str(port),'-uroot','-N','-B'],input='set ob_query_timeout=600000000; '+text,text=True,capture_output=True)
 if r.returncode: raise RuntimeError(r.stderr)
 return r.stdout
if mode=='prepare':
 rng=np.random.default_rng(917)
 data=rng.standard_normal((250000,128),dtype=np.float32)
 with open(root+'sql-vectors.tsv','w') as f:
  for i,row in enumerate(data): f.write(str(1000000+i)+'\t['+','.join(format(float(x),'.9g') for x in row)+']\n')
 np.save(root+'sql-probe.npy',data[42])
elif mode=='load':
 port=int(sys.argv[2]); db='cagra_speed_check'
 sql('create database if not exists '+db+'; use '+db+'; create table items(id bigint primary key, v vector(128)); load data local infile "/workspace/work/sql-vectors.tsv" into table items fields terminated by "\\t" lines terminated by "\\n" (id,v);',port)
 print(sql('select count(*) from '+db+'.items;',port))
elif mode=='build':
 port=int(sys.argv[2]); label=sys.argv[3]
 q='['+','.join(format(float(x),'.9g') for x in np.load(root+'sql-probe.npy'))+']'
 t=time.perf_counter()
 sql('use cagra_speed_check; create vector index idx_items on items(v) with (distance=l2,type=hnsw,lib=cuvs,m=16,ef_construction=200,ef_search=64);',port)
 ddl=time.perf_counter()-t
 ids=sql('select id from cagra_speed_check.items order by l2_distance(v,\''+q+'\') approximate limit 100;',port).split()
 total=time.perf_counter()-t
 assert len(ids)==len(set(ids))==100 and ids[0]=='1000042'
 print(json.dumps(dict(label=label,ddl_s=ddl,ddl_first_query_s=total,rows_returned=len(ids),top1=ids[0])),flush=True)
