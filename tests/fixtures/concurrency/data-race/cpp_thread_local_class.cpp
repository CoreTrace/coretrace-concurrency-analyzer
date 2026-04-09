// SPDX-License-Identifier: Apache-2.0
// Test M2 negative: class instance remains thread-local in each worker
#include <thread>

class Counter
{
  public:
    void increment()
    {
        value++;
    }

  private:
    int value = 0;
};

void worker(int iterations)
{
    Counter local_counter;
    for (int i = 0; i < iterations; ++i)
        local_counter.increment();
}

int main()
{
    std::thread first(worker, 1000);
    std::thread second(worker, 1000);

    first.join();
    second.join();
    return 0;
}
