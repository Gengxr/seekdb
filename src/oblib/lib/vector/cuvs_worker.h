/* Large-stack execution for the optional cuVS backend. */
#pragma once
#include <pthread.h>
#include <condition_variable>
#include <mutex>

namespace oceanbase { namespace common { namespace obvsag {
// A synchronous mailbox: callers keep ownership of their job until run returns.
// Reusing the thread also preserves GPU runtime thread-local state across calls.
class CuvsWorker {
public:
  CuvsWorker() = default;
  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;
  ~CuvsWorker()
  {
    if (started_) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
      }
      ready_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }
  bool run(void *(*fn)(void *), void *arg)
  {
    std::lock_guard<std::mutex> submit(submit_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    if (!started_) {
      pthread_attr_t attr;
      if (pthread_attr_init(&attr) != 0) { return false; }
      int rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
      if (rc == 0) { rc = pthread_create(&thread_, &attr, entry, this); }
      pthread_attr_destroy(&attr);
      if (rc != 0) { return false; }
      started_ = true;
    }
    fn_ = fn;
    arg_ = arg;
    pending_ = true;
    ready_.notify_one();
    done_.wait(lock, [this] { return !pending_; });
    return true;
  }
private:
  static void *entry(void *arg)
  {
    static_cast<CuvsWorker *>(arg)->loop();
    return nullptr;
  }
  void loop()
  {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      ready_.wait(lock, [this] { return pending_ || stop_; });
      if (stop_) { return; }
      auto fn = fn_;
      void *arg = arg_;
      lock.unlock();
      fn(arg);
      lock.lock();
      pending_ = false;
      done_.notify_one();
    }
  }
  pthread_t thread_{};
  std::mutex submit_mutex_, mutex_;
  std::condition_variable ready_, done_;
  void *(*fn_)(void *) = nullptr;
  void *arg_ = nullptr;
  bool started_ = false, pending_ = false, stop_ = false;
};
}}}
