#!/usr/bin/env python3
"""CPU-only round-trip test of the actual adaptor snapshot code with a fake GPU.
This verifies framing/compatibility/cleanup, not GPU performance or recall.
Requires Python 3 and g++ on Linux; writes generated files only to a temp dir.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'src/oblib/lib/vector/ob_vsag_adaptor.cpp').read_text()
snapshot = source[source.index('static const char OB_CUVS_SNAPSHOT_MAGIC'):source.index('}  // anonymous namespace')]
preamble = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <vector>
#include <unistd.h>
#include "cuvs_worker.h"
using oceanbase::common::obvsag::CuvsWorker;
#define LOG_WARN(...) ((void)0)
#define LOG_INFO(...) ((void)0)
constexpr int OB_SUCCESS=0, OB_ERR_UNEXPECTED=-1, OB_ERR_VSAG_RETURN_ERROR=-2;
struct ObTimeUtility { static int64_t current_time() { return 0; } };
static CuvsWorker &ob_cuvs_worker() { static CuvsWorker w; return w; }
struct Fake { long n,dim; std::vector<float> data; };
static int live=0;
static void seekdb_cuvs_free(void *p) { if(p) { delete static_cast<Fake*>(p); --live; } }
static std::shared_ptr<void> ob_cuvs_wrap(void *p) { return {p,seekdb_cuvs_free}; }
static std::shared_ptr<void> ob_cuvs_build_big_stack(const float *p,long n,long d) {
  ++live; return ob_cuvs_wrap(new Fake{n,d,std::vector<float>(p,p+n*d)});
}
static int seekdb_cuvs_serialize(void *p,const char *path) {
  auto &f=*static_cast<Fake*>(p); FILE *out=fopen(path,"wb"); assert(out);
  fwrite(&f.n,sizeof(long),1,out); fwrite(&f.dim,sizeof(long),1,out);
  fwrite(f.data.data(),sizeof(float),f.data.size(),out); fclose(out); return 0;
}
static void *seekdb_cuvs_deserialize(const char *path,long n,long d) {
  FILE *in=fopen(path,"rb"); if(!in) return nullptr;
  long an=0,ad=0; fread(&an,sizeof(long),1,in); fread(&ad,sizeof(long),1,in);
  if(an!=n || ad!=d) { fclose(in); return nullptr; }
  auto *f=new Fake{n,d,std::vector<float>(n*d)};
  size_t read=fread(f->data.data(),sizeof(float),f->data.size(),in); fclose(in);
  if(read!=f->data.size()) { delete f; return nullptr; } ++live; return f;
}
static int seekdb_cuvs_copy_dataset(void *p,float *out,long n,long d) {
  auto &f=*static_cast<Fake*>(p); if(n!=f.n || d!=f.dim) return 1;
  memcpy(out,f.data.data(),f.data.size()*sizeof(float)); return 0;
}
struct ObCuvsEntry {
  std::shared_ptr<void> bridge_; int dim_=0; size_t built_n_=0;
  std::vector<int64_t> ids_,buf_ids_; std::vector<float> buf_vecs_;
};
static std::mutex g_ob_cuvs_mu,g_ob_cuvs_marked_mu;
static std::map<void*,ObCuvsEntry*> g_ob_cuvs_reg;
static std::set<void*> g_ob_cuvs_marked;
static const size_t OB_CUVS_MIN_PTS=256;
'''
test = r'''
int main() {
  int keys[5]{}; auto *e=new ObCuvsEntry; e->dim_=33;
  for(int i=0;i<300;++i) e->buf_ids_.push_back(9000-i*7);
  for(int i=0;i<300*33;++i) e->buf_vecs_.push_back(float(i%71)-35.5f);
  g_ob_cuvs_reg[&keys[0]]=e;
  std::ostringstream out; int rc=99;
  assert(ob_cuvs_serialize(&keys[0],out,rc) && rc==0);
  std::string compact=out.str();
  auto load=[&](std::string bytes,int key) {
    std::istringstream in(bytes.substr(8));
    return ob_cuvs_deserialize(&keys[key],33,in);
  };
  assert(load(compact,1)==0);
  assert(g_ob_cuvs_reg[&keys[1]]->buf_vecs_==e->buf_vecs_);
  assert(g_ob_cuvs_reg[&keys[1]]->ids_==e->buf_ids_);
  std::string legacy=compact; int32_t flag=0;
  memcpy(&legacy[20],&flag,4);
  legacy.insert(32+e->buf_ids_.size()*8,
    reinterpret_cast<const char*>(e->buf_vecs_.data()),e->buf_vecs_.size()*4);
  assert(load(legacy,2)==0);
  assert(g_ob_cuvs_reg[&keys[2]]->buf_vecs_==e->buf_vecs_);
  assert(legacy.size()-compact.size()==e->buf_vecs_.size()*4);
  assert(load(compact.substr(0,compact.size()-1),3)!=0);
  std::string bad=compact; flag=2; memcpy(&bad[20],&flag,4);
  assert(load(bad,3)!=0);
  assert(g_ob_cuvs_reg.count(&keys[3])==0);
  // Appending to a restored index must rebuild from all restored + new vectors.
  auto *restored=g_ob_cuvs_reg[&keys[1]];
  restored->buf_ids_.push_back(-8);
  restored->buf_vecs_.insert(restored->buf_vecs_.end(),33,17.0f);
  std::ostringstream appended;
  assert(ob_cuvs_serialize(&keys[1],appended,rc) && rc==0);
  assert(load(appended.str(),4)==0);
  assert(g_ob_cuvs_reg[&keys[4]]->buf_vecs_==restored->buf_vecs_);
  for(auto &it:g_ob_cuvs_reg) delete it.second;
  g_ob_cuvs_reg.clear(); assert(live==0);
  puts("PASS compact + legacy round-trip, append, truncation, flags, cleanup");
}
'''
with tempfile.TemporaryDirectory(prefix='cagra-snapshot-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    cpp.write_text(preamble + snapshot + test)
    binary = Path(tmp) / 'test'
    subprocess.run(['g++', '-std=c++17', '-pthread', '-fsanitize=address,undefined',
                    '-I', str(root / 'src/oblib/lib/vector'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    # Exercise the actual bridge copy helper, including padded GPU row layout.
    bridge = (root / 'tools/hipvs_bridge/seekdb_cuvs_bridge.c').read_text()
    helper = bridge[bridge.index('int seekdb_cuvs_copy_dataset('):bridge.index('int seekdb_cuvs_cagra_knn(')]
    copy_mock = r'''
#include <pthread.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cassert>
#include <vector>
struct DLTensor {
  void *data; int ndim; int64_t *shape,*strides;
  struct { int code,bits,lanes; } dtype; uint64_t byte_offset;
};
struct DLManagedTensor { DLTensor dl_tensor; void (*deleter)(DLManagedTensor*); };
struct sk_cuvs_index { long n,dim; pthread_mutex_t search_mutex; int res,idx; };
enum { CUVS_SUCCESS=0,kDLFloat=2,cudaMemcpyDefault=0,cudaSuccess=0 };
static DLManagedTensor next_view;
static int deleted=0,fail_copy=0;
static void release(DLManagedTensor*) { ++deleted; }
static int cuvsStreamSync(int) { return 0; }
static int cuvsCagraIndexGetDataset(int,DLManagedTensor *v) { *v=next_view; return 0; }
static int cudaMemcpy(void *d,const void *s,size_t n,int) {
  if(fail_copy) return 1; memcpy(d,s,n); return 0;
}
'''
    copy_test = r'''
int main() {
  sk_cuvs_index h{}; h.n=3; h.dim=5; pthread_mutex_init(&h.search_mutex,nullptr);
  int64_t shape[2]={3,5},strides[2]={8,1};
  std::vector<float> src(25,-999),out(15);
  for(int r=0;r<3;++r) for(int c=0;c<5;++c) src[1+r*8+c]=float(r*5+c);
  next_view={{src.data(),2,shape,strides,{kDLFloat,32,1},4},release};
  assert(seekdb_cuvs_copy_dataset(&h,out.data(),3,5)==0);
  for(int i=0;i<15;++i) assert(out[i]==float(i));
  strides[1]=2; assert(seekdb_cuvs_copy_dataset(&h,out.data(),3,5)!=0);
  strides[1]=1; fail_copy=1; assert(seekdb_cuvs_copy_dataset(&h,out.data(),3,5)!=0);
  fail_copy=0; next_view.dl_tensor.strides=nullptr; next_view.dl_tensor.byte_offset=0;
  assert(seekdb_cuvs_copy_dataset(&h,out.data(),3,5)==0);
  assert(memcmp(out.data(),src.data(),15*sizeof(float))==0);
  assert(deleted==4);
  assert(seekdb_cuvs_copy_dataset(&h,out.data(),2,5)!=0);
  assert(seekdb_cuvs_copy_dataset(nullptr,out.data(),3,5)!=0);
  pthread_mutex_destroy(&h.search_mutex);
}
'''
    cpp.write_text(copy_mock + helper + copy_test)
    subprocess.run(['g++', '-std=c++17', '-pthread', '-fsanitize=address,undefined',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('PASS dataset copy: packed/padded rows, byte offset, invalid layout, copy failure, cleanup')
