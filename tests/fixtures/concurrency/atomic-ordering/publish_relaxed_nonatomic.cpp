// SPDX-License-Identifier: Apache-2.0
// A relaxed flag orders nothing: the consumer may see the flag set and still read the payload the
// producer is writing. The payload is a plain int, so this is a data race.
#include <atomic>
#include <thread>

static int payload = 0;
static std::atomic<bool> ready{false};
static int observed = 0;

static void producer()
{
    payload = 42;
    ready.store(true, std::memory_order_relaxed);
}

static void consumer()
{
    while (!ready.load(std::memory_order_relaxed))
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
