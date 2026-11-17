#include "cuvs_worker.h"
#include "seekdb_cuvs_bridge.h"
#include <cassert>
#include <set>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>
using oceanbase::common::obvsag::CuvsWorker;
struct Job { void *h; const float *q; unsigned ids[100]; float dist[100]; int rc; };
static void *search(void *p) { Job &j=*static_cast<Job *>(p); j.rc=seekdb_cuvs_search(j.h,j.q,1,100,j.ids,j.dist); return nullptr; }
static void *increment(void *p) { ++*static_cast<int *>(p); return nullptr; }
struct Nested { CuvsWorker *worker; int *count; };
static void *nested(void *p) { auto *n=static_cast<Nested *>(p); assert(n->worker->run(increment,n->count)); return nullptr; }
int main()
{
 CuvsWorker worker;
 int count=0;
 std::vector<std::thread> threads;
 for(int t=0;t<8;++t) threads.emplace_back([&]{ for(int i=0;i<1000;++i) assert(worker.run(increment,&count)); });
 for(auto &t:threads)t.join(); assert(count==8000);
 Nested n{&worker,&count}; assert(worker.run(nested,&n)); assert(count==8001);
 for(int i=0;i<20;++i) { CuvsWorker temporary; assert(temporary.run(increment,&count)); }
 assert(count==8021);
 std::vector<float> base(10000*128),query(100*128);
 std::ifstream("/seekdb_workspace/datasets/base.f32",std::ios::binary).read(reinterpret_cast<char*>(base.data()),base.size()*4);
 std::ifstream("/seekdb_workspace/datasets/query.f32",std::ios::binary).read(reinterpret_cast<char*>(query.data()),query.size()*4);
 void *h=seekdb_cuvs_build(base.data(),10000,128); assert(h);
 std::vector<unsigned> reference(100*100);
 for(int i=0;i<100;++i)assert(seekdb_cuvs_search(h,query.data()+i*128,1,100,reference.data()+i*100,nullptr)==0);
 for(int mode: {0,1,1,0}) {
  long overlap=0;
  auto start=std::chrono::steady_clock::now();
  for(int i=0;i<1024;++i){
   Job j{h,query.data()+(i%100)*128,{}, {},-1};
   if(mode) assert(worker.run(search,&j));
   else { pthread_t tid; pthread_attr_t attr; assert(pthread_attr_init(&attr)==0);assert(pthread_attr_setstacksize(&attr,32UL*1024*1024)==0); assert(pthread_create(&tid,&attr,search,&j)==0); assert(pthread_join(tid,nullptr)==0); pthread_attr_destroy(&attr); }
   assert(j.rc==0); std::set<unsigned> found(j.ids,j.ids+100); assert(found.size()==100); for(auto id:found) assert(id<10000); for(int k=0;k<100;++k) overlap+=found.count(reference[(i%100)*100+k]);
  }
  double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
  std::printf("persistent=%d qps=%.3f reference_overlap=%.6f\n",mode,1024/elapsed,double(overlap)/102400); std::fflush(stdout);
 }
 seekdb_cuvs_free(h);
}
