// SPDX-License-Identifier: Apache-2.0
// The payload is a plain int, published through a release store and read after the acquire load
// that observed it: the store happens before the load, so the two accesses to the payload are
// ordered and do not race.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    payload = 42;
    ready.store(true, std::memory_order_release);
}

static void consumer()
{
    while (!ready.load(std::memory_order_acquire))
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
