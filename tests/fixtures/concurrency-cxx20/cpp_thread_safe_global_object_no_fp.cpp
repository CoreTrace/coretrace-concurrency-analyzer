// SPDX-License-Identifier: Apache-2.0
// A thread-safe class keeps its mutex beside its data and takes it in every method. A thread and
// the owner both call the method on one global instance: the method names the lock after `this`,
// and each call site names it after the instance, so both calls hold the same lock.
#include <mutex>
#include <thread>

class Counter
{
  public:
    void increment()
    {
        std::lock_guard<std::mutex> guard(_mutex);
        _value += 1;
    }

  private:
    std::mutex _mutex;
    int _value = 0;
};

Counter counter;

static void work()
{
    counter.increment();
}

int main()
{
    std::thread worker(work);
    counter.increment();
    worker.join();
    return 0;
}
