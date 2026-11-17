#include "../../src/oblib/lib/vector/cuvs_worker.h"
#include <cassert>
#include <thread>
#include <vector>
using oceanbase::common::obvsag::CuvsWorker;
struct Job { int input; int output; pthread_t worker; };
static void *execute(void *arg)
{
  Job &job = *static_cast<Job *>(arg);
  // Exercise more stack than is available on the normal database worker.
  volatile char stack[2 * 1024 * 1024];
  stack[0] = 1;
  stack[sizeof(stack) - 1] = 2;
  job.output = job.input + stack[0] + stack[sizeof(stack) - 1];
  job.worker = pthread_self();
  return nullptr;
}
int main()
{
  for (int lifecycle = 0; lifecycle < 3; ++lifecycle) {
    CuvsWorker worker;
    Job first{7, 0, {}};
    assert(worker.run(execute, &first));
    assert(first.output == 10);
    std::vector<std::thread> callers;
    for (int i = 0; i < 8; ++i) {
      callers.emplace_back([&worker, &first, i] {
        for (int j = 0; j < 100; ++j) {
          Job job{i * 100 + j, 0, {}};
          assert(worker.run(execute, &job));
          assert(job.output == job.input + 3);
          assert(pthread_equal(first.worker, job.worker));
        }
      });
    }
    for (auto &caller : callers) { caller.join(); }
  }
}
