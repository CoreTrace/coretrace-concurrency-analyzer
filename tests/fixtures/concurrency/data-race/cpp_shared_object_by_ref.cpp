// SPDX-License-Identifier: Apache-2.0
// Test M2: shared global object propagated through a reference helper
#include <thread>
#include <vector>

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

Counter global_counter;

void increment_shared(Counter& counter)
{
    counter.increment();
}

void worker(int iterations)
{
    for (int i = 0; i < iterations; ++i)
        increment_shared(global_counter);
}

int main()
{
    std::vector<std::thread> threads;

    for (int i = 0; i < 3; ++i)
        threads.emplace_back(worker, 1000);

    for (auto& thread : threads)
        thread.join();

    return 0;
}
