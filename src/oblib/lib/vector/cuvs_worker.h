#ifndef OCEANBASE_LIB_VECTOR_CUVS_WORKER_H_
#define OCEANBASE_LIB_VECTOR_CUVS_WORKER_H_
#include <pthread.h>
#include <mutex>
#include <condition_variable>

namespace oceanbase { namespace common { namespace obvsag {
// A synchronous executor: callers retain ownership of job arguments until run
// returns. The 32 MiB stack and HIP thread-local state survive between jobs.
class CuvsWorker {
public:
  CuvsWorker() : started_(false), stop_(false), pending_(false), fn_(nullptr), arg_(nullptr)
  {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) == 0) {
      if (pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024) == 0) {
        started_ = pthread_create(&thread_, &attr, entry, this) == 0;
      }
      pthread_attr_destroy(&attr);
    }
  }
  ~CuvsWorker()
  {
    if (started_) {
      { std::lock_guard<std::mutex> lock(state_); stop_ = true; }
      wake_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }
  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;
  bool run(void *(*fn)(void *), void *arg)
  {
    if (!started_ || !fn) return false;
    // Recursive calls already have a large stack and must not wait on themselves.
    if (pthread_equal(pthread_self(), thread_)) { fn(arg); return true; }
    std::lock_guard<std::mutex> submit(submit_);
    std::unique_lock<std::mutex> lock(state_);
    fn_ = fn; arg_ = arg; pending_ = true;
    wake_.notify_one();
    done_.wait(lock, [this] { return !pending_; });
    return true;
  }
private:
  static void *entry(void *arg)
  {
    CuvsWorker *self = static_cast<CuvsWorker *>(arg);
    std::unique_lock<std::mutex> lock(self->state_);
    for (;;) {
      self->wake_.wait(lock, [self] { return self->pending_ || self->stop_; });
      if (self->stop_) break;
      auto fn = self->fn_;
      void *job = self->arg_;
      lock.unlock();
      fn(job);
      lock.lock();
      self->fn_ = nullptr; self->arg_ = nullptr; self->pending_ = false;
      self->done_.notify_one();
    }
    return nullptr;
  }
  pthread_t thread_;
  bool started_, stop_, pending_;
  void *(*fn_)(void *);
  void *arg_;
  std::mutex submit_, state_;
  std::condition_variable wake_, done_;
};
inline bool cuvs_run_on_worker(void *(*fn)(void *), void *arg)
{
  static CuvsWorker worker;
  return worker.run(fn, arg);
}
}}}
#endif
