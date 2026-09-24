// SPDX-License-Identifier: Apache-2.0
// The same publication through an integer flag. A standard library may inline an integer atomic
// even without optimization, leaving one atomic instruction per memory order behind a switch on
// the order; only the arm the constant order selects runs.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<int> ready{0};
static int observed = 0;

static void producer()
{
    payload = 42;
    ready.store(1, std::memory_order_release);
}

static void consumer()
{
    while (ready.load(std::memory_order_acquire) != 1)
    {
    }
    observed = payload;
}

int main()
{
    std::thread first(producer);
    std::thread second(consumer);
    first.join();
    second.join();
    return observed;
}
